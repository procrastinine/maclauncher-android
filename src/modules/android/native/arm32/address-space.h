#pragma once
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>
#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace maclauncher::arm32 {
// Boost owns the balanced free-range indexes. Capacity indexes skip holes
// that cannot satisfy the requested alignment instead of repeatedly walking
// every earlier reservation. The address index owns fixed mappings and merges
// released ranges; this is the same Memory address space as the native aliases.
class FreeAddressRanges {
  struct Range {uint64_t begin,end;};
  struct Address {};
  template<unsigned Alignment> struct Capacity {
    using result_type=uint64_t;
    uint64_t operator()(const Range &range)const{
      const auto aligned=(range.begin+Alignment-1)&~uint64_t(Alignment-1);
      return aligned<range.end?range.end-aligned:0;
    }
  };
  template<unsigned Alignment> using Sized=boost::multi_index::ordered_non_unique<
    boost::multi_index::tag<Capacity<Alignment>>,boost::multi_index::composite_key<Range,
      Capacity<Alignment>,boost::multi_index::member<Range,uint64_t,&Range::begin>>>;
  using Ranges=boost::multi_index_container<Range,boost::multi_index::indexed_by<
    boost::multi_index::ordered_unique<boost::multi_index::tag<Address>,boost::multi_index::member<Range,uint64_t,&Range::begin>>,
    Sized<16>,Sized<32>,Sized<64>,Sized<128>,Sized<256>,Sized<512>,Sized<1024>,Sized<2048>,Sized<4096>>>;
  Ranges ranges{{Range{0,UINT64_C(0x100000000)}}};
  template<unsigned Alignment> uint32_t place(uint32_t preferred,uint32_t size)const{
    const auto &capacity=ranges.template get<Capacity<Alignment>>();
    for(auto next=capacity.lower_bound(boost::make_tuple(uint64_t(size)));next!=capacity.end();++next){
      const auto aligned=(std::max(next->begin,uint64_t(preferred))+Alignment-1)&~uint64_t(Alignment-1);
      if(aligned<=next->end&&size<=next->end-aligned)return uint32_t(aligned);
    }
    throw std::runtime_error("ARM32 address space exhausted");
  }
public:
  uint32_t place(uint32_t preferred,uint32_t size,uint32_t alignment)const{
    if(!size)throw std::runtime_error("Invalid ARM32 allocation size");
    switch(alignment){
      case 16:return place<16>(preferred,size);
      case 32:return place<32>(preferred,size);
      case 64:return place<64>(preferred,size);
      case 128:return place<128>(preferred,size);
      case 256:return place<256>(preferred,size);
      case 512:return place<512>(preferred,size);
      case 1024:return place<1024>(preferred,size);
      case 2048:return place<2048>(preferred,size);
      case 4096:return place<4096>(preferred,size);
      default:throw std::runtime_error("Invalid ARM32 allocation alignment");
    }
  }
  void reserve(uint32_t address,uint32_t size){
    const uint64_t begin=address,end=begin+size;
    auto &ordered=ranges.get<Address>();auto next=ordered.upper_bound(begin);
    if(!size||next==ordered.begin())throw std::runtime_error("Overlapping ARM32 mapping");
    --next;const auto first=next->begin,last=next->end;
    if(end>last)throw std::runtime_error("Overlapping ARM32 mapping");
    ordered.erase(next);
    if(first<begin)ordered.insert({first,begin});
    if(end<last)ordered.insert({end,last});
  }
  void release(uint32_t address,uint32_t size){
    uint64_t begin=address,end=begin+size;
    auto &ordered=ranges.get<Address>();auto next=ordered.lower_bound(begin);
    if(next!=ordered.begin()){const auto prior=std::prev(next);if(prior->end==begin){begin=prior->begin;ordered.erase(prior);}}
    if(next!=ordered.end()&&next->begin==end){end=next->end;ordered.erase(next);}
    ordered.insert({begin,end});
  }
};
} // namespace maclauncher::arm32
