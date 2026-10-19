import fs from 'node:fs';
import path from 'node:path';

function replace(text, before, after) {
  if (text.split(before).length !== 2)
    throw Error('Unexpected Dynarmic code-location source: '+before.slice(0,80));
  return text.replace(before, after);
}
function write(file, text) {
  fs.mkdirSync(path.dirname(file), {recursive:true});
  if (!fs.existsSync(file) || fs.readFileSync(file,'utf8') !== text)
    fs.writeFileSync(file, text);
  return file;
}

// Keep cache registration, compiled blocks and their lifetime with Dynarmic.
// Only compilation/clear touches the metadata guard; generated code is intact.
export function projectCodeLocationException(text) {
  text=replace(text,'#include <thread>','#include <thread>\n#include "code-location.h"');
  text=replace(text,'    auto FindCodeBlockInfo(u64 rip) {','    std::vector<CodeBlockInfo>::iterator FindCodeBlockInfo(u64 rip) {');
  text=replace(text,'    std::function<FakeCall(u64)> cb;','    std::function<FakeCall(u64)> cb;\n    std::shared_ptr<std::recursive_mutex> metadata = std::make_shared<std::recursive_mutex>();\n    KljArm32CodeQuery query = nullptr;\n    void* query_context = nullptr;');
  text=replace(text,'    void RemoveCodeBlock(u64 rip);',`    void RemoveCodeBlock(u64 rip);
    void InstallCodeQuery(u64 base, KljArm32CodeQuery query, void* context) {
        std::lock_guard guard(code_block_infos_mutex);
        auto it = FindCodeBlockInfo(base);
        if (it != code_block_infos.end() && it->code_begin == base) {
            it->query = query;
            it->query_context = context;
        }
    }
    void* EnterCodeMetadata(u64 base) {
        std::lock_guard guard(code_block_infos_mutex);
        auto it = FindCodeBlockInfo(base);
        if (it == code_block_infos.end() || it->code_begin != base) return nullptr;
        it->metadata->lock();
        return it->metadata.get();
    }
    int QueryCodeLocation(u64 pc, KljArm32CodeLocation* result) {
        std::unique_lock guard(code_block_infos_mutex, std::try_to_lock);
        if (!guard.owns_lock()) return 2;
        auto it = FindCodeBlockInfo(pc);
        if (it == code_block_infos.end()) return 0;
        *result = {it->code_begin, it->code_end, 0, 0, 0};
        // A query must not wait while the emitting thread holds metadata and
        // enters a nested cache clear through this same registration owner.
        std::unique_lock metadata(*it->metadata, std::try_to_lock);
        if (!metadata.owns_lock()) return 2;
        return it->query && it->query(it->query_context, pc, result) ? 1 : 3;
    }`);
  text+=`
extern "C" void maclauncher_dynarmic_install_code_query(
    uint64_t base, KljArm32CodeQuery query, void* context) {
    using namespace Dynarmic::Backend;
    std::lock_guard guard(handler_lock);
    if (mach_handler) mach_handler->InstallCodeQuery(base, query, context);
}
extern "C" void* maclauncher_dynarmic_enter_code_metadata(uint64_t base) {
    using namespace Dynarmic::Backend;
    std::lock_guard guard(handler_lock);
    return mach_handler ? mach_handler->EnterCodeMetadata(base) : nullptr;
}
extern "C" void maclauncher_dynarmic_leave_code_metadata(void* lock) {
    if (lock) static_cast<std::recursive_mutex*>(lock)->unlock();
}
extern "C" __attribute__((visibility("default"))) int
maclauncher_arm32_code_location(uint64_t pc, KljArm32CodeLocation* result) {
    if (!result) return 0;
    *result = {};
    using namespace Dynarmic::Backend;
    std::unique_lock guard(handler_lock, std::try_to_lock);
    if (!guard.owns_lock()) return 2;
    return mach_handler ? mach_handler->QueryCodeLocation(pc, result) : 0;
}
`;
  return text;
}

export function prepareCodeLocationSources({source, output}) {
  const backend=path.join(source,'src/dynarmic/backend/arm64');
  let address=fs.readFileSync(path.join(backend,'address_space.cpp'),'utf8');
  address=replace(address,'#include <cstdio>','#include <cstdio>\n#include "code-location.h"');
  address=replace(address,'void AddressSpace::ClearCache() {','void AddressSpace::ClearCache() {\n    KljArm32CodeMetadataScope metadata(mcl::bit_cast<u64>(mem.ptr()));');
  address=replace(address,'EmittedBlockInfo AddressSpace::Emit(IR::Block block) {','EmittedBlockInfo AddressSpace::Emit(IR::Block block) {\n    KljArm32CodeMetadataScope metadata(mcl::bit_cast<u64>(mem.ptr()));');
  address=replace(address,'        return FastmemCallback(host_pc);\n    });','        return FastmemCallback(host_pc);\n    });'+`
    maclauncher_dynarmic_install_code_query(mcl::bit_cast<u64>(mem.ptr()),
        [](void* context, uint64_t pc, KljArm32CodeLocation* result) -> int {
            auto& self = *static_cast<AddressSpace*>(context);
            const auto pointer = mcl::bit_cast<CodePtr>(pc);
            const auto entry = self.ReverseGetEntryPoint(pointer);
            if (!entry) return 0;
            const auto block = self.block_infos.find(entry);
            if (block == self.block_infos.end() || pointer < entry ||
                size_t(pointer - entry) >= block->second.size) return 0;
            const auto location = self.ReverseGetLocation(pointer);
            if (!location) return 0;
            result->entry = mcl::bit_cast<uint64_t>(entry);
            result->size = block->second.size;
            result->descriptor = location->Value();
            return 1;
        }, this);
`);
  return {
    address:write(path.join(output,'address_space.cpp'),address),
  };
}
if (process.argv[1] && path.resolve(process.argv[1])===import.meta.filename)
  prepareCodeLocationSources({source:path.resolve(process.argv[2]),output:path.resolve(process.argv[3])});
