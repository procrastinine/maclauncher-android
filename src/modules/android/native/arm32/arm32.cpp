#include "arm32.h"
#include "address-space.h"
#include "page-table.h"
#include "../image-format.h"
#include "bionic-packed.h"
#include "kernel-helpers.inc"
#include "dynarmic/interface/A32/a32.h"
#include "dynarmic/interface/A32/coprocessor.h"
#include "dynarmic/interface/exclusive_monitor.h"
#include <boost/icl/interval_map.hpp>
#include <boost/icl/set.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <stdexcept>
#include <sys/mman.h>
#include <unistd.h>

namespace maclauncher::arm32 {
namespace {
constexpr uint32_t page = 4096;
constexpr uint32_t compactAlignment=16;
std::runtime_error memoryError(const char* reason,uint32_t address,size_t size,unsigned permission){
  char detail[192];std::snprintf(detail,sizeof detail,"%s: address=0x%08x bytes=%zu permission=%u",reason,address,size,permission);
  return std::runtime_error(detail);
}
uint32_t add(uint32_t a, uint32_t b) {
  if (b > UINT32_MAX - a)
    throw std::runtime_error("ARM32 address overflow");
  return a + b;
}
uint32_t align(uint32_t n) { return add(n, page - 1) & ~(page - 1); }
template <class T> T read(const void *address) {
  T value;
  std::memcpy(&value, address, sizeof value);
  return value;
}
template <class T> void write(void *address, T value) {
  std::memcpy(address, &value, sizeof value);
}
using MappingMutex=std::shared_mutex;
struct Region : PageTableMapping {
  std::shared_ptr<void> owner;
  std::function<void(unsigned)> prepareProtection;
  std::string origin;
  bool shared=false;
  std::vector<uint8_t> permissions;
  Region(uint32_t base, uint32_t size, unsigned permissions)
      : PageTableMapping{base,size,size,nullptr},
        permissions(size / page, permissions) {
    permissionBytes=this->permissions.data();
    bytes = static_cast<uint8_t *>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANON, -1, 0));
    if (bytes == MAP_FAILED)
      throw std::bad_alloc();
    owner=std::shared_ptr<void>(bytes,[size](void *value){munmap(value,size);});
  }
  Region(uint32_t base, uint32_t size, unsigned permissions, void *borrowed,
         uint32_t accessible)
      : PageTableMapping{base,size,accessible,static_cast<uint8_t *>(borrowed)},
        permissions((uint64_t(size) + page - 1) / page, permissions) {
    permissionBytes=this->permissions.data();
  }
  Region(const Region &other,uint32_t base,uint32_t size)
      :PageTableMapping{base,size,base-other.base<other.accessible?std::min(size,other.accessible-(base-other.base)):0,
       reinterpret_cast<uint8_t *>(uintptr_t(other.bytes)+(base-other.base))},owner(other.owner),prepareProtection(other.prepareProtection),origin(other.origin),shared(other.shared),
       permissions(other.permissions.begin()+(base-other.base)/page,
                   other.permissions.begin()+(uint64_t(base)-other.base+size+page-1)/page) {
    permissionBytes=permissions.data();
  }
};
// The bounded hot working set belongs to one guarded Run, never to a Region
// lifetime. The sparse page index below resolves misses without a global walk.
struct ExecutionMappings {
  std::array<Region *,8> entries{};
  unsigned used=0;
  bool overflow=false;
  Region *find(uint32_t address){
    for(unsigned i=0;i<used;++i){
      Region *region=entries[i];
      if(address>=region->base && uint64_t(address)-region->base<region->size){
        if(i)std::swap(entries[0],entries[i]);
        return region;
      }
    }
    return nullptr;
  }
  void remember(Region *region){
    // A working set that exceeds this cache uses the page index directly.
    // Repeated linear misses must not add another cost to every buffer access.
    if(overflow)return;
    if(used==entries.size()){overflow=true;used=0;return;}
    const auto slot=used++;
    entries[slot]=entries[0];entries[0]=region;
  }
};
} // namespace
struct Memory::Impl {
  mutable MappingMutex mutex;
  std::mutex unalignedExclusive;
  std::map<uint32_t, std::unique_ptr<Region>> regions;
  // The ordered map alone owns mappings. This sparse, derived page index
  // narrows compact-buffer lookup to the original 16-byte reservation cells
  // on one guest page. Every mapping is page- or ABI-reservation-aligned.
  // Only the first/last pages need entries: large complete interior pages use
  // Dynarmic's existing direct table, with the owning map as the cold fallback.
  // Index storage therefore scales with mappings, never reserved virtual size.
  // Readers retain the same mapping guard; removal clears every borrowed
  // pointer before the original Region owner can be retired.
  struct PageEntries {
    std::array<PageTableMapping *,page/compactAlignment> cells{};
    unsigned used=0;
  };
  struct PageGroup {std::array<std::unique_ptr<PageEntries>,1024> pages;unsigned used=0;};
  std::array<std::unique_ptr<PageGroup>,1024> guestPages;
  FreeAddressRanges free;
  // Dynarmic's non-owning page lookup belongs to this same mapping owner.
  // Complete readable/writable pages use original direct pointers. A tagged
  // edge-page entry refers to the existing compact reservation cells; the same
  // Dynarmic lookup reads their original bounds and permissions inline.
  using PageTable=std::array<uint8_t *,Dynarmic::A32::UserConfig::NUM_PAGE_TABLE_ENTRIES>;
  std::unique_ptr<PageTable> pageTable=std::make_unique<PageTable>();
  // Several guest mappings may alias the same native bytes. Each interval
  // records guest bases in address order, matching the original first match.
  using NativeRanges=boost::icl::interval_map<uintptr_t,std::set<uint32_t>>;
  NativeRanges nativeRanges;
  std::map<void *, std::function<void(uint32_t, uint32_t)>> observers;
  std::vector<HostCall> calls{HostCall{}};
  std::vector<uint32_t> gates;
  uint32_t place(uint32_t preferred,uint32_t size,uint32_t alignment=page) const {
    try {return free.place(preferred,size,alignment);}
    catch(const std::runtime_error &error) {
      // This is allocation-failure detail, not another registry or a hot-path
      // counter. Native capacity and guest reservations are distinct quantities.
      uint64_t reservedBytes=0,accessibleBytes=0;
      for(const auto &[base,region]:regions){
        (void)base;reservedBytes+=region->size;accessibleBytes+=region->accessible;
      }
      throw std::runtime_error(std::string(error.what())+
        ": request="+std::to_string(size)+" alignment="+std::to_string(alignment)+
        " mappings="+std::to_string(regions.size())+
        " reserved="+std::to_string(reservedBytes)+
        " accessible="+std::to_string(accessibleBytes));
    }
  }
  void index(const Region &region,bool inserting) {
    if(!region.accessible)return;
    const uintptr_t begin=reinterpret_cast<uintptr_t>(region.bytes);
    const uintptr_t extent=region.accessible-1;
    const uintptr_t end=extent>UINTPTR_MAX-begin?UINTPTR_MAX:begin+extent;
    NativeRanges::segment_type range{NativeRanges::interval_type::closed(begin,end),{region.base}};
    if(inserting)nativeRanges.add(range);else nativeRanges.subtract(range);
  }
  void pageEntry(const Region &region,uint64_t address,bool inserting){
    uint8_t *bytes=nullptr;
    if(inserting&&address>=region.base){
      const uint64_t offset=address-region.base;
      if(offset<region.accessible&&page<=region.accessible-offset){
        const auto first=offset/page,last=(offset+page-1)/page;
        if((region.permissions[first]&(Read|Write))==(Read|Write)&&
           (region.permissions[last]&(Read|Write))==(Read|Write)&&
           !(uintptr_t(region.bytes+offset)&boundedPageTag))bytes=region.bytes+offset;
      }
    }
    if(inserting&&!bytes){
      const auto number=address/page;
      const auto &group=guestPages[number/1024];
      const auto *entries=group?group->pages[number%1024].get():nullptr;
      if(entries)bytes=reinterpret_cast<uint8_t *>(uintptr_t(entries->cells.data())|boundedPageTag);
    }
    std::atomic_ref((*pageTable)[address/page]).store(bytes,std::memory_order_release);
  }
  void pageEntries(const Region &region,bool inserting){
    const uint64_t first=(uint64_t(region.base)+page-1)&~uint64_t(page-1);
    const uint64_t end=(uint64_t(region.base)+region.accessible)&~uint64_t(page-1);
    for(auto address=first;address<end;address+=page)pageEntry(region,address,inserting);
  }
  void guestPageEntries(Region &region,bool inserting){
    const std::array<uint32_t,2> pages={region.base/page,uint32_t((uint64_t(region.base)+region.size-1)/page)};
    for(unsigned i=0;i<(pages[0]==pages[1]?1u:2u);++i){
      const auto number=pages[i];
      auto &group=guestPages[number/1024];
      const auto first=unsigned((std::max(uint64_t(region.base),uint64_t(number)*page)%page)/compactAlignment);
      const auto end=unsigned((std::min(uint64_t(region.base)+region.size,(uint64_t(number)+1)*page)-uint64_t(number)*page+compactAlignment-1)/compactAlignment);
      if(inserting){
        if(!group)group=std::make_unique<PageGroup>();
        auto &entries=group->pages[number%1024];
        if(!entries){entries=std::make_unique<PageEntries>();++group->used;}
        std::fill(entries->cells.begin()+first,entries->cells.begin()+end,&region);
        entries->used+=end-first;
        std::atomic_ref((*pageTable)[number]).store(reinterpret_cast<uint8_t *>(uintptr_t(entries->cells.data())|boundedPageTag),std::memory_order_release);
      }else{
        auto &entries=group->pages[number%1024];
        std::fill(entries->cells.begin()+first,entries->cells.begin()+end,nullptr);
        entries->used-=end-first;
        std::atomic_ref((*pageTable)[number]).store(entries->used?reinterpret_cast<uint8_t *>(uintptr_t(entries->cells.data())|boundedPageTag):nullptr,std::memory_order_release);
        if(!entries->used){entries.reset();if(!--group->used)group.reset();}
      }
    }
  }
  void insert(std::unique_ptr<Region> region,bool publishCode=true) {
    auto [entry,inserted]=regions.emplace(region->base,std::move(region));
    if(!inserted)throw std::runtime_error("Overlapping ARM32 mapping");
    free.reserve(entry->first,entry->second->size);
    index(*entry->second,true);
    guestPageEntries(*entry->second,true);
    pageEntries(*entry->second,true);
    // A prior denied fetch can itself have a translated NoExecuteFault block.
    // Publishing executable storage must retire that result at a reused address.
    if(publishCode)invalidateCode(*entry->second,entry->first,entry->second->size);
  }
  void invalidateCode(const Region &region,uint32_t address,uint32_t size) const {
    const uint64_t begin=std::max(uint64_t(address),uint64_t(region.base));
    const uint64_t end=std::min(uint64_t(address)+size,uint64_t(region.base)+region.accessible);
    if(begin>=end)return;
    const auto first=(begin-region.base)/page,last=(end-region.base+page-1)/page;
    if(std::none_of(region.permissions.begin()+first,region.permissions.begin()+last,
                    [](unsigned permission){return permission&Execute;}))return;
    // Code fetch requires Execute. Removing a data-only buffer cannot retire
    // compiled instructions; its mapping/page-table lifetime is still updated.
    for(const auto &[owner,notify]:observers){(void)owner;notify(address,size);}
  }
  std::unique_ptr<Region> remove(decltype(regions)::iterator entry) {
    pageEntries(*entry->second,false);
    guestPageEntries(*entry->second,false);
    index(*entry->second,false);
    free.release(entry->first,entry->second->size);
    auto retired=std::move(entry->second);
    regions.erase(entry);
    return retired;
  }
  void map(uint32_t address,uint32_t size,unsigned permission) {
    if(!size || address%page || size%page || uint64_t(address)+size>UINT64_C(0x100000000))
      throw std::runtime_error("Invalid ARM32 mapping");
    auto next=regions.lower_bound(address);
    if(next!=regions.end() && uint64_t(address)+size>next->first)
      throw std::runtime_error("Overlapping ARM32 mapping");
    if(next!=regions.begin()){
      const auto &prior=*std::prev(next)->second;
      if(uint64_t(prior.base)+prior.size>address)throw std::runtime_error("Overlapping ARM32 mapping");
    }
    insert(std::make_unique<Region>(address,size,permission));
  }
  uint32_t allocate(uint32_t preferred,uint32_t size,unsigned permission) {
    size=align(size);const auto address=place(preferred,size);map(address,size,permission);return address;
  }
  std::vector<std::unique_ptr<Region>> removeRange(uint32_t address,uint32_t size) {
    std::vector<std::unique_ptr<Region>> retired;
    const uint64_t end=uint64_t(address)+size;
    auto it=regions.upper_bound(address);if(it!=regions.begin())--it;
    while(it!=regions.end() && it->first<end){
      auto &region=*it->second;const uint64_t finish=uint64_t(region.base)+region.size;
      if(finish<=address){++it;continue;}
      std::unique_ptr<Region> before,after;
      if(region.base<address)before=std::make_unique<Region>(region,region.base,address-region.base);
      if(finish>end)after=std::make_unique<Region>(region,uint32_t(end),uint32_t(finish-end));
      const auto removed=uint32_t(std::max(uint64_t(address),uint64_t(region.base)));
      invalidateCode(region,removed,uint32_t(std::min(end,finish)-removed));
      auto next=std::next(it);retired.push_back(remove(it));it=next;
      if(before)insert(std::move(before),false);
      if(after){insert(std::move(after),false);break;}
    }
    return retired;
  }
  uint32_t returnGate() {
    if(gates.empty()){
      const auto address=allocate(0x7f000000,page,Read|Execute);gates.push_back(address);
      const uint32_t code[]={0xe300c000u,0xe340c000u,0xef00007fu,0xe12fff1eu};
      std::memcpy(translate(address,sizeof code,0),code,sizeof code);
    }
    return gates.front();
  }
  HostCall hostCall(unsigned index) const {
    if(index>=calls.size() || !calls[index])
      throw std::runtime_error("Unknown or retired ARM32 host gate");
    return calls[index];
  }
  // Caller holds mutex until the returned bytes have been read or written.
  void *translate(uint32_t address,size_t size,unsigned permission,ExecutionMappings *execution=nullptr) const {
    Region *selected=execution?execution->find(address):nullptr;
    if(!selected){
      const auto entry=uintptr_t((*pageTable)[address/page]);
      if(entry&boundedPageTag){
        const auto *cells=reinterpret_cast<PageTableMapping *const *>(entry&~boundedPageTag);
        selected=static_cast<Region *>(cells[(address%page)/compactAlignment]);
      }
      if(!selected){
        const auto next=regions.upper_bound(address);
        if(next!=regions.begin())selected=std::prev(next)->second.get();
      }
      if(selected&&execution)execution->remember(selected);
    }
    if(!selected||uint64_t(address)-selected->base>=selected->size)throw Fault(memoryError("Unmapped ARM32 address",address,size,permission).what());
    Region &region=*selected;
    const uint64_t offset=uint64_t(address)-region.base;
    auto fault=[&](const char *reason){
      char detail[192];std::snprintf(detail,sizeof detail," region=0x%08x span=%u accessible=%u",region.base,region.size,region.accessible);
      return Fault(std::string(memoryError(reason,address,size,permission).what())+detail+
        (region.origin.empty()?"":" origin="+region.origin));
    };
    if(offset>=region.accessible || size>region.accessible-offset)throw fault("ARM32 access crosses mapping");
    if(size)for(size_t i=offset/page;i<=(offset+size-1)/page;++i)
      if((region.permissions[i]&permission)!=permission)throw fault("ARM32 memory protection fault");
    return region.bytes+offset;
  }
};
Memory::Memory() : impl(std::make_unique<Impl>()) {
  map(0xffff0000, sizeof kernelHelpers, Read | Execute);
  std::memcpy(loadAddress(0xffff0000, sizeof kernelHelpers), kernelHelpers,
              sizeof kernelHelpers);
}
Memory::~Memory() = default;
void Memory::map(uint32_t address, uint32_t size, unsigned permission) {
  std::lock_guard lock(impl->mutex);
  impl->map(address,size,permission);
}
uint32_t Memory::allocate(uint32_t preferred, uint32_t size,
                          unsigned permission) {
  std::lock_guard lock(impl->mutex);
  return impl->allocate(preferred,size,permission);
}
void Memory::unmap(uint32_t address) {
  std::unique_ptr<Region> retired;
  {
  std::lock_guard lock(impl->mutex);
  auto found = impl->regions.find(address);
  if (found == impl->regions.end())
    return;
  impl->invalidateCode(*found->second,address,found->second->size);
  retired=impl->remove(found);
  }
}
void Memory::unmap(uint32_t address,uint32_t size) {
  if(!size || address%page || size%page || uint64_t(address)+size>UINT64_C(0x100000000))
    throw std::runtime_error("Invalid ARM32 unmapping");
  std::vector<std::unique_ptr<Region>> retired;
  {
    std::lock_guard lock(impl->mutex);retired=impl->removeRange(address,size);
  }
}
void Memory::observe(void *owner,
                     std::function<void(uint32_t, uint32_t)> notify) {
  std::lock_guard lock(impl->mutex);
  impl->observers.emplace(owner, std::move(notify));
}
void Memory::stopObserving(void *owner) {
  std::lock_guard lock(impl->mutex);
  impl->observers.erase(owner);
}
void Memory::invalidate(uint32_t address,uint32_t size) {
  std::shared_lock lock(impl->mutex);
  for(const auto &[owner,notify]:impl->observers){(void)owner;notify(address,size);}
}
void *Memory::translate(uint32_t address, size_t size,
                        unsigned permission) const {
  std::shared_lock lock(impl->mutex);
  return impl->translate(address,size,permission);
}
void *Memory::loadAddress(uint32_t address, size_t size) const {
  return translate(address, size, 0);
}
void Memory::protect(uint32_t address, uint32_t size, unsigned permission) {
  std::lock_guard lock(impl->mutex);
  if ((address % page) || (size % page) || uint64_t(address)+size>UINT64_C(0x100000000))
    throw std::runtime_error("Unaligned ARM32 protection");
  const uint64_t end=uint64_t(address)+size;
  // Validate the complete interval before publishing any changed protection.
  for(uint64_t at=address;at<end;at+=page)impl->translate(uint32_t(at),page,0);
  for(uint64_t at=address;at<end;){
    Region &region=*std::prev(impl->regions.upper_bound(uint32_t(at)))->second;
    if(region.prepareProtection)region.prepareProtection(permission);
    at=std::min(end,uint64_t(region.base)+region.size);
  }
  for(uint64_t at=address;at<end;at+=page){
    Region &region=*std::prev(impl->regions.upper_bound(uint32_t(at)))->second;
    region.permissions[(at-region.base)/page]=permission;
    impl->pageEntry(region,at,true);
  }
  for (const auto &[owner, notify] : impl->observers) {
    (void)owner;
    notify(address, size);
  }
}
std::string Memory::string(uint32_t address) const {
  std::shared_lock lock(impl->mutex);
  std::string value;
  for (;;) {
    char c = read<char>(impl->translate(address, 1, Read));
    if (!c)
      return value;
    value += c;
    address = add(address, 1);
  }
}

uint32_t Memory::returnGate() {
  std::lock_guard lock(impl->mutex);
  return impl->returnGate();
}
uint32_t Memory::import(HostCall call) {
  std::lock_guard lock(impl->mutex);
  impl->returnGate();
  if (impl->calls.size() > UINT32_MAX)
    throw std::runtime_error("ARM32 call-gate address space exhausted");
  uint32_t index = uint32_t(impl->calls.size());
  if (index % 256 == 0)
    impl->gates.push_back(impl->allocate(0x7f000000, page, Read | Execute));
  uint32_t address = add(impl->gates[index / 256], (index % 256) * 16);
  uint32_t code[] = {0xe300c000u | ((index & 0xf000) << 4) | (index & 0xfff),
                     0xe340c000u | ((index & 0xf0000000) >> 12) |
                         ((index >> 16) & 0xfff),
                     0xef00007fu, 0xe12fff1eu};
  std::memcpy(impl->translate(address, sizeof code,0), code, sizeof code);
  impl->calls.push_back(std::move(call));
  return address;
}
void Memory::retireImport(uint32_t address) {
  std::lock_guard lock(impl->mutex);
  for (size_t pageIndex = 0; pageIndex < impl->gates.size(); pageIndex++) {
    auto base = impl->gates[pageIndex];
    if (address < base || address >= base + page || (address - base) % 16)
      continue;
    auto index = pageIndex * 256 + (address - base) / 16;
    if (index < impl->calls.size())
      impl->calls[index] = {};
    return;
  }
}
uint64_t Memory::invoke(unsigned index, Thread &thread) {
  HostCall call;
  {
    std::shared_lock lock(impl->mutex);
    call=impl->hostCall(index);
  }
  return call(thread);
}
uint32_t Memory::borrow(void *bytes, uint32_t size, unsigned permission,std::string_view origin) {
  if (!bytes)
    return 0;
  std::lock_guard lock(impl->mutex);
  // Native owners already allocate these bytes. Their guest representation
  // needs ABI alignment and exact bounds, not a separate virtual page for each
  // scalar, string, JNI buffer or heap allocation. Keep the native alignment
  // up to the page alignment that this representation has always supplied.
  const uint32_t alignment=std::max(compactAlignment,1u<<std::min(12u,unsigned(std::countr_zero(reinterpret_cast<uintptr_t>(bytes)))));
  const auto reserved=(add(std::max(1u,size),compactAlignment-1)&~(compactAlignment-1));
  auto address = impl->place(0x50000000, reserved, alignment);
  auto region=std::make_unique<Region>(address,reserved,permission,bytes,std::max(1u,size));
  region->origin=origin.substr(0,192);
  impl->insert(std::move(region));
  return address;
}
uint32_t Memory::mapNative(void *bytes,uint32_t size,unsigned permission,
                          std::shared_ptr<void> owner,uint32_t preferred,
                          bool fixed,bool noReplace,std::function<void(unsigned)> prepareProtection,bool shared) {
  if(!bytes || !size)throw std::runtime_error("Invalid ARM32 native mapping");
  std::vector<std::unique_ptr<Region>> retired;
  std::lock_guard lock(impl->mutex);auto reserved=align(size);
  uint32_t address;
  if(fixed){
    if(!preferred || preferred%page || uint64_t(preferred)+reserved>UINT64_C(0x100000000))
      throw std::runtime_error("Invalid ARM32 fixed mapping");
    if(noReplace){
      for(const auto &[base,region]:impl->regions)if(uint64_t(base)<uint64_t(preferred)+reserved && uint64_t(base)+region->size>preferred)
        throw std::runtime_error("Occupied ARM32 fixed mapping");
    }
    address=preferred;retired=impl->removeRange(address,reserved);
  }else address=impl->place(preferred?(preferred&~(page-1)):0x50000000,reserved);
  auto region=std::make_unique<Region>(address,reserved,permission,bytes,reserved);
  region->owner=std::move(owner);region->prepareProtection=std::move(prepareProtection);region->shared=shared;
  impl->insert(std::move(region));return address;
}
uint32_t Memory::addressOf(const void *bytes) const {
  std::shared_lock lock(impl->mutex);
  auto address = reinterpret_cast<uintptr_t>(bytes);
  auto found=impl->nativeRanges.find(address);
  if(found==impl->nativeRanges.end())return 0;
  const auto base=*found->second.begin();
  const auto &region=*impl->regions.find(base)->second;
  return base+uint32_t(address-reinterpret_cast<uintptr_t>(region.bytes));
}
std::string Memory::mappingDescription(uint32_t address) const {
  std::shared_lock lock(impl->mutex);
  auto next=impl->regions.upper_bound(address);
  if(next==impl->regions.begin())return "unmapped";
  const auto &region=*std::prev(next)->second;
  if(uint64_t(address)-region.base>=region.size)return "unmapped";
  char extent[128];std::snprintf(extent,sizeof extent,"base=0x%08x span=%u accessible=%u",region.base,region.size,region.accessible);
  return std::string(extent)+(region.origin.empty()?"":" origin="+region.origin);
}
std::vector<Memory::Mapping> Memory::mappings() const {
  std::vector<Mapping> result;
  std::shared_lock lock(impl->mutex);
  for(const auto &[base,owned]:impl->regions){
    const auto &region=*owned;uint64_t offset=0;
    while(offset<region.accessible){
      const auto permission=region.permissions[size_t(offset/page)];
      uint64_t end=std::min(uint64_t(region.accessible),(offset/page+1)*page);
      while(end<region.accessible&&region.permissions[size_t(end/page)]==permission)
        end=std::min(uint64_t(region.accessible),end+page);
      result.push_back({uint64_t(base)+offset,uint64_t(base)+end,permission,unsigned(region.shared)});
      offset=end;
    }
  }
  return result;
}
void Memory::retireNative(const void *bytes,size_t size) {
  if(!bytes||!size)return;
  std::vector<std::unique_ptr<Region>> retired;
  {
    std::lock_guard lock(impl->mutex);
    const uintptr_t begin=reinterpret_cast<uintptr_t>(bytes);
    const uintptr_t end=size-1>UINTPTR_MAX-begin?UINTPTR_MAX:begin+size-1;
    const auto range=Impl::NativeRanges::interval_type::closed(begin,end);
    std::set<uint32_t> bases;
    auto finish=impl->nativeRanges.upper_bound(range);
    for(auto it=impl->nativeRanges.lower_bound(range);it!=finish;++it)
      bases.insert(it->second.begin(),it->second.end());
    for(auto base:bases){
      auto found=impl->regions.find(base);if(found==impl->regions.end())continue;
      impl->invalidateCode(*found->second,base,found->second->size);
      retired.push_back(impl->remove(found));
    }
  }
}

struct Thread::Impl final : Dynarmic::A32::UserCallbacks {
  struct Tls final : Dynarmic::A32::Coprocessor {
    uint32_t value = 0;
    std::optional<Callback> CompileInternalOperation(bool, unsigned,
                                                     Dynarmic::A32::CoprocReg,
                                                     Dynarmic::A32::CoprocReg,
                                                     Dynarmic::A32::CoprocReg,
                                                     unsigned) override {
      return {};
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned op,
                                               Dynarmic::A32::CoprocReg n,
                                               Dynarmic::A32::CoprocReg m,
                                               unsigned op2) override {
      return !two && !op && n == Dynarmic::A32::CoprocReg::C13 &&
                     m == Dynarmic::A32::CoprocReg::C0 && op2 == 2
                 ? CallbackOrAccessOneWord{&value}
                 : CallbackOrAccessOneWord{};
    }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned op,
                                              Dynarmic::A32::CoprocReg n,
                                              Dynarmic::A32::CoprocReg m,
                                              unsigned op2) override {
      return !two && !op && n == Dynarmic::A32::CoprocReg::C13 &&
                     m == Dynarmic::A32::CoprocReg::C0 && (op2 == 2 || op2 == 3)
                 ? CallbackOrAccessOneWord{&value}
                 : CallbackOrAccessOneWord{};
    }
    CallbackOrAccessTwoWords
    CompileSendTwoWords(bool, unsigned, Dynarmic::A32::CoprocReg) override {
      return {};
    }
    CallbackOrAccessTwoWords
    CompileGetTwoWords(bool, unsigned, Dynarmic::A32::CoprocReg) override {
      return {};
    }
    std::optional<Callback> CompileLoadWords(bool, bool,
                                             Dynarmic::A32::CoprocReg,
                                             std::optional<uint8_t>) override {
      return {};
    }
    std::optional<Callback> CompileStoreWords(bool, bool,
                                              Dynarmic::A32::CoprocReg,
                                              std::optional<uint8_t>) override {
      return {};
    }
  };
  Thread &owner;
  Memory &memory;
  uint32_t stackBase = 0, stackTop = 0, gateBase = 0;
  uint64_t ticks = 0;
  std::vector<uint32_t> ownedGates;
  std::function<int64_t(Thread &, uint32_t)> syscall;
  std::shared_ptr<Tls> tls = std::make_shared<Tls>();
  Dynarmic::ExclusiveMonitor monitor{1};
  std::unique_ptr<Dynarmic::A32::Jit> cpu;
  std::string error;
  uint32_t pending = 0;
  bool pendingSyscall = false;
  unsigned depth = 0;
  const bool traceThreads = [] {
    const char *value = std::getenv("KL_TRACE_THREADS");
    return value && *value && std::strcmp(value, "0");
  }();
  std::atomic_flag reentryLogged = ATOMIC_FLAG_INIT;
  uintptr_t diagnosticImageBase = 0;
  MappingMutex &memoryLock;
  ExecutionMappings *executionMappings=nullptr;
  Impl(Thread &owner, Memory &memory, uint32_t base, uint32_t size)
      : owner(owner), memory(memory), memoryLock(memory.impl->mutex) {
    if (traceThreads) {
      Dl_info image{};
      if (dladdr(reinterpret_cast<const void *>(memoryError), &image))
        diagnosticImageBase = reinterpret_cast<uintptr_t>(image.dli_fbase);
    }
    stackBase = memory.allocate(base, size, Memory::Read | Memory::Write);
    stackTop = add(stackBase, align(size));
    gateBase = memory.returnGate();
    Dynarmic::A32::UserConfig config{};
    config.callbacks = this;
    config.arch_version = Dynarmic::A32::ArchVersion::v7;
    config.coprocessors[15] = tls;
    config.global_monitor = &monitor;
    config.check_halt_on_memory_access = true;
    config.always_little_endian = true;
    config.page_table=memory.impl->pageTable.get();
    config.page_table_pointer_mask_bits=1;
    config.detect_misaligned_access_via_page_table=8|16|32|64;
    config.only_detect_misalignment_via_page_table_on_page_boundary=true;
    cpu = std::make_unique<Dynarmic::A32::Jit>(config);
    memory.observe(this, [this](uint32_t address, uint32_t bytes) {
      cpu->InvalidateCacheRange(address, bytes);
    });
  }
  ~Impl() {
    memory.stopObserving(this);
    cpu.reset();
    memory.unmap(stackBase);
    for (auto address : ownedGates)
      memory.retireImport(address);
  }
  void clearCallHalt() {
    // These reasons belong to this call's fault, return and native boundary.
    // Cache invalidation and any independently requested stop retain their
    // original owners across call entry and recursive-state restoration.
    cpu->ClearHalt(Dynarmic::HaltReason::MemoryAbort |
                   Dynarmic::HaltReason::UserDefined1 |
                   Dynarmic::HaltReason::UserDefined2);
  }
  void fault(const std::string &message) {
    if(!error.empty())return;
    char location[224];
    const auto regs=cpu?cpu->Regs():std::array<uint32_t,16>{};
    std::snprintf(location,sizeof location," (pc=0x%08x lr=0x%08x r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x r12=0x%08x sp=0x%08x)",
      regs[15],regs[14],regs[0],regs[1],regs[2],regs[3],regs[12],regs[13]);
    error = message+location;
    if (cpu)
      cpu->HaltExecution(Dynarmic::HaltReason::MemoryAbort);
  }
  template <class T> T load(uint32_t address) {
    try {
      return read<T>(memory.impl->translate(address, sizeof(T), Memory::Read,executionMappings));
    } catch (const std::exception &e) {
      fault(e.what());
      return 0;
    }
  }
  template <class T> void store(uint32_t address, T value) {
    try {
      write(memory.impl->translate(address, sizeof(T), Memory::Write,executionMappings), value);
    } catch (const std::exception &e) {
      fault(e.what());
    }
  }
  template <class T> bool exclusive(uint32_t address, T value, T expected) {
    try {
      if(!error.empty())return false;
      auto *bytes=memory.impl->translate(address,sizeof(T),Memory::Read|Memory::Write,executionMappings);
      if(reinterpret_cast<uintptr_t>(bytes)%std::atomic_ref<T>::required_alignment){
        // Compact aliases can have naturally aligned guest words over an
        // unaligned native buffer. Retain that representation with a standard
        // serialized compare/write when native atomic_ref cannot address it.
        std::lock_guard lock(memory.impl->unalignedExclusive);
        if(read<T>(bytes)!=expected)return false;
        write(bytes,value);return true;
      }
      return std::atomic_ref(*static_cast<T *>(bytes)).compare_exchange_strong(expected,value);
    }catch(const std::exception &e){fault(e.what());return false;}
  }
  std::optional<uint32_t> MemoryReadCode(uint32_t address) override {
    try {
      return read<uint32_t>(memory.impl->translate(address, 4, Memory::Execute,executionMappings));
    } catch (...) {
      return {};
    }
  }
  uint8_t MemoryRead8(uint32_t a) override { return load<uint8_t>(a); }
  uint16_t MemoryRead16(uint32_t a) override { return load<uint16_t>(a); }
  uint32_t MemoryRead32(uint32_t a) override { return load<uint32_t>(a); }
  uint64_t MemoryRead64(uint32_t a) override { return load<uint64_t>(a); }
  void MemoryWrite8(uint32_t a, uint8_t v) override { store(a, v); }
  void MemoryWrite16(uint32_t a, uint16_t v) override { store(a, v); }
  void MemoryWrite32(uint32_t a, uint32_t v) override { store(a, v); }
  void MemoryWrite64(uint32_t a, uint64_t v) override { store(a, v); }
  bool MemoryWriteExclusive8(uint32_t a, uint8_t v, uint8_t e) override {
    return exclusive(a, v, e);
  }
  bool MemoryWriteExclusive16(uint32_t a, uint16_t v, uint16_t e) override {
    return exclusive(a, v, e);
  }
  bool MemoryWriteExclusive32(uint32_t a, uint32_t v, uint32_t e) override {
    return exclusive(a, v, e);
  }
  bool MemoryWriteExclusive64(uint32_t a, uint64_t v, uint64_t e) override {
    return exclusive(a, v, e);
  }
  void InterpreterFallback(uint32_t pc, size_t) override {
    fault("ARM32 instruction needs interpreter at " + std::to_string(pc));
  }
  void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception kind) override {
    fault("ARM32 exception " + std::to_string(unsigned(kind)) + " at " +
          std::to_string(pc));
  }
  void CallSVC(uint32_t number) override {
    if (number == 0x7f) {
      pending = cpu->Regs()[12];
      pendingSyscall = false;
      cpu->HaltExecution(pending ? Dynarmic::HaltReason::UserDefined2
                                 : Dynarmic::HaltReason::UserDefined1);
    } else if (!number) {
      pendingSyscall = true;
      cpu->HaltExecution(Dynarmic::HaltReason::UserDefined2);
    } else
      fault("Unsupported ARM32 SVC convention");
  }
  void AddTicks(uint64_t used) override {
    ticks = used >= ticks ? 0 : ticks - used;
  }
  uint64_t GetTicksRemaining() override {
    // Dynarmic's ARM64 dispatcher tests the remaining counter as signed.
    // Keep the public uint64_t budget, but expose only a positive signed
    // execution window; otherwise UINT64_MAX returns after every block.
    return std::min(ticks,uint64_t(INT64_MAX));
  }
};
Thread::Thread(Memory &memory, uint32_t base, uint32_t size)
    : impl(std::make_unique<Impl>(*this, memory, base, size)) {}
Thread::~Thread() = default;
Memory &Thread::memory() { return impl->memory; }
uint32_t Thread::tls() const { return impl->tls->value; }
void Thread::setTls(uint32_t value) { impl->tls->value = value; }
void Thread::setSyscall(std::function<int64_t(Thread &, uint32_t)> call) {
  impl->syscall = std::move(call);
}
uint32_t Thread::import(HostCall call) {
  auto address = impl->memory.import(std::move(call));
  impl->ownedGates.push_back(address);
  return address;
}
uint32_t Thread::argument(unsigned word) const {
  return word < 4 ? impl->cpu->Regs()[word]
                  : read<uint32_t>(impl->memory.translate(
                        add(impl->cpu->Regs()[13], (word - 4) * 4), 4,
                        Memory::Read));
}
void Thread::arguments(unsigned first,std::span<uint32_t> words) const {
  if(words.empty())return;
  if(words.size()>UINT32_MAX || uint64_t(first)+words.size()>UINT32_MAX)
    throw std::out_of_range("ARM32 argument span");
  size_t copied=0;
  for(;copied<words.size()&&uint64_t(first)+copied<4;++copied)
    words[copied]=impl->cpu->Regs()[first+copied];
  if(copied==words.size())return;
  const auto stackWord=uint64_t(first)+copied-4;
  const auto byteCount=(words.size()-copied)*sizeof(uint32_t);
  if(stackWord>UINT32_MAX/4 || byteCount>UINT32_MAX)
    throw std::out_of_range("ARM32 stack argument span");
  // AAPCS arguments are contiguous after r0-r3. Validate their actual span
  // once under the same mapping owner instead of acquiring it for each word.
  std::shared_lock lock(impl->memoryLock);
  std::memcpy(words.data()+copied,impl->memory.impl->translate(
      add(impl->cpu->Regs()[13],uint32_t(stackWord*4)),byteCount,Memory::Read),byteCount);
}
uint32_t Thread::syscallArgument(unsigned word) const {
  if(word>=7)throw std::out_of_range("ARM EABI syscall argument register");
  return impl->cpu->Regs()[word];
}
uint32_t Thread::returnAddress() const { return impl->cpu->Regs()[14]; }
uint64_t Thread::call(uint32_t entry, std::span<const Value> arguments,
                      uint64_t budget) {
  auto &cpu = *impl->cpu;
  if (impl->traceThreads && cpu.IsExecuting() &&
      !impl->reentryLogged.test_and_set(std::memory_order_relaxed)) {
    // This ingress can be reached from a POSIX callback. Record the first
    // active-engine entry without allocation, symbol lookup or stdio locks;
    // ordinary execution and Dynarmic's own invariant remain unchanged.
    char line[256];size_t length = 0;
    auto text = [&](const char *value) {while (*value && length < sizeof line)line[length++] = *value++;};
    auto hex = [&](uint64_t value) {text("0x");for (int digit=15;digit>=0;--digit)if(length<sizeof line)line[length++]="0123456789abcdef"[(value>>(digit*4))&15];};
    text("[arm32] active-engine reentry entry=");hex(entry);
    text(" stored-pc=");hex(cpu.Regs()[15]);text(" stored-lr=");hex(cpu.Regs()[14]);
    text(" caller-offset=");hex(reinterpret_cast<uintptr_t>(__builtin_return_address(0))-impl->diagnosticImageBase);
    text("\n");(void)::write(STDERR_FILENO,line,length);
  }
  auto regs = cpu.Regs();
  auto vectors = cpu.ExtRegs();
  auto cpsr = cpu.Cpsr(), fpscr = cpu.Fpscr();
  uint64_t previousTicks = impl->ticks;
  uint32_t top = impl->depth ? regs[13] : impl->stackTop;
  ++impl->depth;
  struct Restore {
    Dynarmic::A32::Jit &cpu;
    decltype(regs) &regs;
    decltype(vectors) &vectors;
    uint32_t cpsr, fpscr;
    Impl &impl;
    uint64_t ticks;
    ~Restore() {
      cpu.Regs() = regs;
      cpu.ExtRegs() = vectors;
      cpu.SetCpsr(cpsr);
      cpu.SetFpscr(fpscr);
      impl.clearCallHalt();
      impl.ticks = ticks;
      --impl.depth;
    }
  } restore{cpu, regs, vectors, cpsr, fpscr, *impl, previousTicks};
  std::vector<uint32_t> words;
  for (auto value : arguments) {
    if (value.words != 1 && value.words != 2)
      throw std::runtime_error("Invalid ARM32 argument width");
    const unsigned alignment=value.alignment?value.alignment:(value.words==2?8:4);
    if (alignment > 4 && words.size() % 2)
      words.push_back(0);
    words.push_back(uint32_t(value.bits));
    if (value.words == 2)
      words.push_back(uint32_t(value.bits >> 32));
  }
  uint32_t stackBytes = words.size() > 4 ? uint32_t((words.size() - 4) * 4) : 0;
  if (top < stackBytes + 16)
    throw std::runtime_error("ARM32 stack underflow");
  uint32_t sp = (top - stackBytes - 16) & ~7u;
  cpu.Regs().fill(0);
  for (size_t i = 0; i < std::min<size_t>(4, words.size()); i++)
    cpu.Regs()[i] = words[i];
  if (stackBytes)
    std::memcpy(impl->memory.translate(sp, stackBytes, Memory::Write),
                words.data() + 4, stackBytes);
  cpu.Regs()[13] = sp;
  cpu.Regs()[14] = impl->gateBase;
  cpu.Regs()[15] = entry & ~1u;
  cpu.SetCpsr(0x10u | ((entry & 1u) ? 0x20u : 0));
  impl->clearCallHalt();
  impl->ticks = budget;
  impl->error.clear();
  while (impl->ticks) {
    // All Dynarmic memory callbacks and direct page accesses stay within this
    // read-side mapping lifetime. SVCs halt Run before any native invocation,
    // so callbacks, allocation and recursive guest entry acquire fresh guards.
    Dynarmic::HaltReason reason;
    Memory::HostCall nativeCall;
    {
      std::shared_lock lock(impl->memoryLock);
      ExecutionMappings mappings;
      struct MappingScope {
        Impl &thread;
        MappingScope(Impl &thread,ExecutionMappings &mappings):thread(thread){thread.executionMappings=&mappings;}
        ~MappingScope(){thread.executionMappings=nullptr;}
      } mappingScope{*impl,mappings};
      reason=cpu.Run();
      if(impl->error.empty() && Dynarmic::Has(reason,Dynarmic::HaltReason::UserDefined2) && !impl->pendingSyscall)
        nativeCall=impl->memory.impl->hostCall(impl->pending);
    }
    if (!impl->error.empty())
      throw std::runtime_error(impl->error);
    if (Dynarmic::Has(reason, Dynarmic::HaltReason::UserDefined1))
      return uint64_t(cpu.Regs()[0]) | (uint64_t(cpu.Regs()[1]) << 32);
    if (Dynarmic::Has(reason, Dynarmic::HaltReason::UserDefined2)) {
      uint64_t result;
      const bool syscall=impl->pendingSyscall;
      if (syscall) {
        if (!impl->syscall)
          throw std::runtime_error("ARM32 syscall adapter unavailable");
        result = uint64_t(impl->syscall(*this, cpu.Regs()[7]));
      } else {
        result = nativeCall(*this);
      }
      cpu.Regs()[0] = uint32_t(result);
      if(!syscall)cpu.Regs()[1] = uint32_t(result >> 32);
      cpu.ClearHalt(Dynarmic::HaltReason::UserDefined2);
    }
  }
  throw std::runtime_error("ARM32 instruction budget exhausted");
}

namespace {
struct Ehdr {
  uint8_t ident[16];
  uint16_t type, machine;
  uint32_t version, entry, phoff, shoff, flags;
  uint16_t ehsize, phsize, phnum, shsize, shnum, shstr;
};
struct Phdr {
  uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, alignment;
};
struct Dyn {
  int32_t tag;
  uint32_t value;
};
struct Sym {
  uint32_t name, value, size;
  uint8_t info, other;
  uint16_t section;
};
struct Rel {
  uint32_t offset, info;
};
static_assert(sizeof(Ehdr) == 52 && sizeof(Phdr) == 32 && sizeof(Sym) == 16);
template <class T> T fileRead(std::span<const uint8_t> bytes, size_t offset) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("Truncated ELF32 record");
  return read<T>(bytes.data() + offset);
}
} // namespace
struct Image::Impl {
  Memory &memory;
  uint32_t bias, symbols = 0, strings = 0, stringSize = 0, count = 0, init = 0,
                 initArray = 0, initBytes = 0;
  uint32_t mappedSpan = 0;
  uint32_t hashBuckets=0,hashChains=0,hashBucketCount=0;
  uint32_t gnuBuckets=0,gnuChains=0,gnuBloom=0,gnuBucketCount=0,
           gnuFirst=0,gnuBloomCount=0,gnuShift=0;
  using Header64=KlProgramHeader;
  std::vector<Header64> headers;
  std::vector<std::string> dependencies;
  std::vector<uint32_t> importGates;
  std::map<int32_t, uint32_t> dynamic;
  Resolve resolve;
  unsigned linkState=0;
  std::optional<uint32_t> mapping;
  Impl(Memory &memory, uint32_t bias, Resolve resolve)
      : memory(memory), bias(bias), resolve(std::move(resolve)) {}
  ~Impl() {
    for (auto address : importGates)
      memory.retireImport(address);
    if (mapping)
      memory.unmap(*mapping);
  }
  uint32_t at(uint32_t value) const { return add(bias, value); }
  std::string symbolName(uint32_t offset) const {
    if (offset >= stringSize)
      throw std::runtime_error("ELF32 string offset outside table");
    auto value = memory.string(add(strings, offset));
    if (value.size() >= stringSize - offset)
      throw std::runtime_error("Unterminated ELF32 name");
    return value;
  }
  Sym symbolAt(uint32_t index) const {
    if (index >= count)
      throw std::runtime_error("ELF32 symbol index outside table");
    return read<Sym>(
        memory.loadAddress(add(symbols, index * sizeof(Sym)), sizeof(Sym)));
  }
  uint32_t word(uint32_t table,uint32_t index)const{
    if(index>UINT32_MAX/4)throw std::runtime_error("ELF32 hash table overflow");
    return read<uint32_t>(memory.loadAddress(add(table,index*4),4));
  }
  uint32_t findSymbol(std::string_view name)const{
    // The original dynamic tables narrow candidates; Image remains their
    // sole lifetime owner. Keep the existing lowest-defined-index selection
    // for aliases, without changing process/group search order.
    const std::string terminated(name);
    uint32_t best=UINT32_MAX;
    const auto match=[&](uint32_t index){const auto value=symbolAt(index);if(value.section&&symbolName(value.name)==name)best=std::min(best,index);};
    if(gnuBucketCount){
      const uint32_t hash=bionic::calculate_gnu_hash_simple(terminated.c_str()).first;
      const uint32_t bloom=word(gnuBloom,(hash/32)%gnuBloomCount);
      if(!(bloom&(uint32_t(1)<<(hash%32)))||!(bloom&(uint32_t(1)<<((hash>>gnuShift)%32))))return 0;
      uint32_t index=word(gnuBuckets,hash%gnuBucketCount);
      if(!index)return 0;
      for(;;index++){
        if(index<gnuFirst||index>=count)throw std::runtime_error("Invalid ELF32 GNU hash chain");
        const uint32_t chain=word(gnuChains,index-gnuFirst);
        if(((chain^hash)>>1)==0)match(index);
        if(chain&1)break;
      }
    }else if(hashBucketCount){
      const uint32_t hash=bionic::calculate_elf_hash(terminated.c_str());
      uint32_t index=word(hashBuckets,hash%hashBucketCount);
      for(uint32_t visited=0;index;visited++){
        if(index>=count||visited>=count)throw std::runtime_error("Invalid ELF32 hash chain");
        match(index);index=word(hashChains,index);
      }
    }
    if(best==UINT32_MAX)return 0;
    const auto value=symbolAt(best);return value.section==0xfff1?value.value:at(value.value);
  }
  uint32_t address(const Sym &symbol,bool functionImport=false) {
    if (symbol.section)
      return symbol.section == 0xfff1 ? symbol.value : at(symbol.value);
    auto name = symbolName(symbol.name);
    uint32_t result = resolve ? resolve(name) : 0;
    if (!result && (symbol.info >> 4) != 2) {
      if ((symbol.info & 15) == 2 || functionImport) {
        result = memory.import([name](Thread &) -> uint64_t {
          throw std::runtime_error("ARM32 native import missing: " + name);
        });
        importGates.push_back(result);
      } else
        throw std::runtime_error("ARM32 native data import missing: " + name);
    }
    return result;
  }
  void relocation(Rel rel) {
      uint32_t type=rel.info&255;
      if(!type)return;
      uint32_t location=at(rel.offset),value=read<uint32_t>(memory.loadAddress(location,4));
      uint32_t symbol=(rel.info>>8)?address(symbolAt(rel.info>>8),type==22):0;
      switch(type) {
      case 2:value+=symbol;break;
      case 3:value+=symbol-location;break;
      case 21:case 22:value=symbol;break;
      case 23:value+=bias;break;
      default:throw std::runtime_error("ARM32 relocation not implemented: "+std::to_string(type));
      }
      write(memory.loadAddress(location,4),value);
  }
  void relocate(uint32_t start, uint32_t bytes) {
    if (bytes % sizeof(Rel))
      throw std::runtime_error("Invalid ELF32 relocation size");
    for (uint32_t i = 0; i < bytes; i += sizeof(Rel)) {
      Rel rel = read<Rel>(memory.loadAddress(add(start, i), sizeof(Rel)));
      relocation(rel);
    }
  }
  void packed(uint32_t start,uint32_t bytes) {
    auto *data=static_cast<const uint8_t *>(memory.loadAddress(start,bytes));
    if(bytes<4 || std::memcmp(data,"APS2",4))throw std::runtime_error("Invalid Android packed relocation header");
    bionic::for_all_packed_relocs(bionic::sleb128_decoder(data+4,bytes-4),[this](bionic::Elf32_Rel rel){relocation({rel.r_offset,rel.r_info});return true;});
  }
  void relr(uint32_t start,uint32_t bytes) {
    if(bytes%4)throw std::runtime_error("Invalid ELF32 RELR size");
    uint32_t location=0;
    for(uint32_t i=0;i<bytes;i+=4) {
      uint32_t entry=read<uint32_t>(memory.loadAddress(add(start,i),4));
      if(!(entry&1)){location=entry;relocation({location,23});location=add(location,4);}
      else {
        for(unsigned bit=1;bit<32;bit++)if(entry&(uint32_t(1)<<bit))relocation({add(location,(bit-1)*4),23});
        location=add(location,31*4);
      }
    }
  }
};
Image::Layout Image::inspect(std::span<const uint8_t> bytes) {
  auto header=fileRead<Ehdr>(bytes,0);
  if(std::memcmp(header.ident,"\177ELF",4) || header.ident[4]!=1 || header.ident[5]!=1 ||
     header.machine!=40 || header.type!=3 || header.phsize!=sizeof(Phdr))
    throw std::runtime_error("Expected little-endian ARM ELF32 shared object");
  Layout result{};std::vector<Phdr> loads;Phdr dynamic{};
  for(unsigned i=0;i<header.phnum;i++) {
    auto p=fileRead<Phdr>(bytes,uint64_t(header.phoff)+i*sizeof(Phdr));
    if(p.offset>bytes.size() || p.filesz>bytes.size()-p.offset)throw std::runtime_error("Truncated ELF32 segment");
    if(p.type==1){loads.push_back(p);result.span=std::max(result.span,align(add(p.vaddr,p.memsz)));}
    if(p.type==2)dynamic=p;
  }
  uint32_t strings=0,stringSize=0;std::vector<uint32_t> needed;
  for(uint32_t i=0;i+sizeof(Dyn)<=dynamic.filesz;i+=sizeof(Dyn)) {
    auto d=fileRead<Dyn>(bytes,uint64_t(dynamic.offset)+i);if(!d.tag)break;
    if(d.tag==5)strings=d.value;
    if(d.tag==10)stringSize=d.value;
    if(d.tag==1)needed.push_back(d.value);
  }
  for(uint32_t offset:needed) {
    if(offset>=stringSize)throw std::runtime_error("ELF32 dependency name exceeds string table");
    uint32_t address=add(strings,offset);const char *name=nullptr;size_t limit=0;
    for(auto p:loads)if(address>=p.vaddr && uint64_t(address)<uint64_t(p.vaddr)+p.filesz) {
      size_t at=p.offset+size_t(address-p.vaddr);name=reinterpret_cast<const char *>(bytes.data()+at);
      limit=std::min(size_t(stringSize-offset),size_t(p.filesz-(address-p.vaddr)));break;
    }
    if(!name || !std::memchr(name,0,limit))throw std::runtime_error("Unterminated ELF32 dependency name");
    result.needed.emplace_back(name);
  }
  return result;
}
Image::Image(Memory &memory, std::span<const uint8_t> bytes, uint32_t bias,
             Resolve resolver)
    : Image(memory,bytes,bias) { relocate(std::move(resolver)); }
Image::Image(Memory &memory, std::span<const uint8_t> bytes, uint32_t bias)
    : impl(std::make_unique<Impl>(memory, bias, Resolve{})) {
  Ehdr header = fileRead<Ehdr>(bytes, 0);
  if (std::memcmp(header.ident,
                  "\x7f"
                  "ELF",
                  4) ||
      header.ident[4] != 1 || header.ident[5] != 1 || header.type != 3 ||
      header.machine != 40 || header.phsize != sizeof(Phdr) ||
      header.ehsize != sizeof(Ehdr))
    throw std::runtime_error("Expected little-endian ARM ELF32 shared object");
  std::vector<Phdr> segments;
  uint32_t low = UINT32_MAX, high = 0;
  Phdr dynamics{};
  for (unsigned i = 0; i < header.phnum; i++) {
    auto segment =
        fileRead<Phdr>(bytes, uint64_t(header.phoff) + i * sizeof(Phdr));
    impl->headers.push_back({segment.type,segment.flags,segment.offset,segment.vaddr,segment.paddr,segment.filesz,segment.memsz,segment.alignment});
    if (segment.type == 2)
      dynamics = segment;
    if (segment.type != 1)
      continue;
    if (segment.filesz > segment.memsz || segment.offset > bytes.size() ||
        segment.filesz > bytes.size() - segment.offset)
      throw std::runtime_error("Invalid ELF32 load segment");
    if (!segment.memsz)
      continue;
    low = std::min(low, segment.vaddr & ~(page - 1));
    high = std::max(high, align(add(segment.vaddr, segment.memsz)));
    segments.push_back(segment);
  }
  if (!high || low >= high)
    throw std::runtime_error("ELF32 has no loadable memory");
  uint32_t mapping = add(bias, low);
  memory.map(mapping, high - low, 0);
  impl->mapping = mapping;
  impl->mappedSpan=high;
  for (auto segment : segments) {
    std::memcpy(memory.loadAddress(add(bias, segment.vaddr), segment.filesz),
                bytes.data() + segment.offset, segment.filesz);
    memory.protect(add(bias, segment.vaddr & ~(page - 1)),
                   align(add(segment.vaddr % page, segment.memsz)),
                   segment.flags & 7);
  }
  std::vector<uint32_t> needed;
  for (uint32_t i = 0; i + sizeof(Dyn) <= dynamics.filesz; i += sizeof(Dyn)) {
    Dyn value = read<Dyn>(
        memory.loadAddress(add(bias, add(dynamics.vaddr, i)), sizeof(Dyn)));
    if (!value.tag)
      break;
    if (value.tag == 1)
      needed.push_back(value.value);
    else
      impl->dynamic[value.tag] = value.value;
  }
  auto &d = impl->dynamic;
  impl->symbols = impl->at(d[6]);
  impl->strings = impl->at(d[5]);
  impl->stringSize = d[10];
  if (d[11] != sizeof(Sym) || !impl->stringSize)
    throw std::runtime_error("Invalid ELF32 symbol tables");
  if (d[4]){
    const uint32_t hash=impl->at(d[4]);
    impl->hashBucketCount=impl->word(hash,0);impl->count=impl->word(hash,1);
    if(impl->hashBucketCount>UINT32_MAX/4)throw std::runtime_error("ELF32 hash table overflow");
    impl->hashBuckets=add(hash,8);impl->hashChains=add(impl->hashBuckets,impl->hashBucketCount*4);
    memory.loadAddress(impl->hashBuckets,uint64_t(impl->hashBucketCount)*4);
    if(impl->count)memory.loadAddress(impl->hashChains,uint64_t(impl->count)*4);
  }
  else if (d[0x6ffffef5]) {
    uint32_t hash = impl->at(d[0x6ffffef5]);
    auto n = read<uint32_t>(memory.loadAddress(hash, 4)),
         first = read<uint32_t>(memory.loadAddress(add(hash, 4), 4)),
         bloom = read<uint32_t>(memory.loadAddress(add(hash, 8), 4));
    if (bloom > UINT32_MAX / 4 || n > UINT32_MAX / 4)
      throw std::runtime_error("ELF32 hash table overflow");
    uint32_t buckets = add(hash, add(16, bloom * 4)),
             chains = add(buckets, n * 4);
    impl->gnuBucketCount=n;impl->gnuFirst=first;impl->gnuBloomCount=bloom;
    impl->gnuBloom=add(hash,16);impl->gnuBuckets=buckets;impl->gnuChains=chains;
    impl->gnuShift=impl->word(hash,3);
    if(!n||!bloom||impl->gnuShift>=32)throw std::runtime_error("Invalid ELF32 GNU hash table");
    memory.loadAddress(impl->gnuBloom,uint64_t(bloom)*4);memory.loadAddress(buckets,uint64_t(n)*4);
    impl->count=first;
    for (uint32_t i = 0; i < n; i++) {
      uint32_t index =
          read<uint32_t>(memory.loadAddress(add(buckets, i * 4), 4));
      if (!index)
        continue;
      if (index < first)
        throw std::runtime_error("Invalid ELF32 GNU hash");
      for (;; index++) {
        if (index - first > UINT32_MAX / 4)
          throw std::runtime_error("ELF32 hash chain overflow");
        uint32_t value = read<uint32_t>(
            memory.loadAddress(add(chains, (index - first) * 4), 4));
        impl->count = std::max(impl->count, add(index, 1));
        if (value & 1)
          break;
      }
    }
    if(impl->count<first)throw std::runtime_error("Invalid ELF32 GNU hash symbol extent");
    if(impl->count>first)memory.loadAddress(chains,uint64_t(impl->count-first)*4);
  } else
    throw std::runtime_error("ELF32 dynamic symbol count unavailable");
  memory.loadAddress(impl->symbols, uint64_t(impl->count) * sizeof(Sym));
  memory.loadAddress(impl->strings, impl->stringSize);
  for (auto offset : needed)
    impl->dependencies.push_back(impl->symbolName(offset));
  impl->init = d[12] ? impl->at(d[12]) : 0;
  impl->initArray = d[25] ? impl->at(d[25]) : 0;
  impl->initBytes = d[27];
}
void Image::relocate(Resolve resolver) {
  if(impl->linkState==2)return;
  if(impl->linkState)throw std::runtime_error("ELF32 relocation did not complete");
  impl->linkState=1;impl->resolve=std::move(resolver);
  auto &d=impl->dynamic;
  if (d[17]) {
    if (d[19] != sizeof(Rel))
      throw std::runtime_error("Unsupported ELF32 REL format");
    impl->relocate(impl->at(d[17]), d[18]);
  }
  if (d[23]) {
    if (d[20] != 17)
      throw std::runtime_error("Unsupported ELF32 PLT relocation format");
    impl->relocate(impl->at(d[23]), d[2]);
  }
  if(d[0x6000000f])impl->packed(impl->at(d[0x6000000f]),d[0x60000010]);
  if(d[36])impl->relr(impl->at(d[36]),d[35]);
  if(d[0x6fffe000])impl->relr(impl->at(d[0x6fffe000]),d[0x6fffe001]);
  if(d[7] || d[0x60000011])throw std::runtime_error("ARM EABI expects REL relocations instead of RELA");
  // The original guest page owner changes permissions only after every
  // relocation has consumed its writable targets, before any constructor.
  for(const auto &header:impl->headers) {
    if(header.type!=0x6474e552 || !header.memsz)continue;
    uint32_t start=uint32_t(header.vaddr)&~(page-1);
    uint32_t end=align(add(uint32_t(header.vaddr),uint32_t(header.memsz)));
    impl->memory.protect(add(impl->bias,start),end-start,Memory::Read);
  }
  impl->linkState=2;
  impl->resolve={};
}
Image::~Image() = default;
std::string Image::runpath()const{auto found=impl->dynamic.find(29);return found==impl->dynamic.end()?std::string():impl->symbolName(found->second);}
uint32_t Image::symbol(std::string_view name) const {
  return impl->findSymbol(name);
}
void Image::initialize(Thread &thread) {
    if(impl->linkState!=2)throw std::runtime_error("ELF32 constructors require completed relocation");
    if (impl->init)
      thread.call(impl->init);
    if (impl->initBytes % 4)
      throw std::runtime_error("Invalid ELF32 initializer array");
    for (uint32_t i = 0; i < impl->initBytes; i += 4) {
      uint32_t address =
          read<uint32_t>(impl->memory.loadAddress(add(impl->initArray, i), 4));
      if (address && address != UINT32_MAX)
        thread.call(address);
    }
}
const std::vector<std::string> &Image::dependencies() const {
  return impl->dependencies;
}
uint32_t Image::base() const { return impl->bias; }
uint32_t Image::span() const { return impl->mappedSpan; }
const void *Image::programHeaders(unsigned *count) const {
  if(count)*count=unsigned(impl->headers.size());
  return impl->headers.data();
}
} // namespace maclauncher::arm32
