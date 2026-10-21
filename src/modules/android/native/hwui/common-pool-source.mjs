import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
const hashes={"CommonPool.h":"dc2711bacdd058e739ef842fb04670f002aa3e8734820fe506abb937ed60d969","CommonPool.cpp":"9d62ca2567ae7c8db1957dd85b23c37b00adc75742862c39bb9e78d0abe8d45b"};
const exact=(text,from,to)=>{if(text.split(from).length!==2)throw Error('Original HWUI CommonPool source differs from its pinned input');return text.replace(from,to);};
export function adaptCommonPool({header,source}) {
for(const [name,text] of [['CommonPool.h',header],['CommonPool.cpp',source]])if(crypto.createHash('sha256').update(text).digest('hex')!==hashes[name])throw Error('Original HWUI CommonPool source identity differs: '+name);
const owner=String.raw`
namespace {
thread_local bool retireCommonPoolWorker = false;
// Constructed during the pool constructor, before the pool's own destruction
// registration. Worker ownership outlives the pool and adds no object fields.
struct CommonPoolWorkers {
    std::array<std::thread, CommonPool::THREAD_COUNT> threads;

    void join() {
        const auto self = std::this_thread::get_id();
        std::thread* caller = nullptr;
        for (auto& thread : threads) {
            if (thread.joinable() && thread.get_id() == self) {
                // Global/DSO finalization can originate in a pool task. The
                // caller cannot join itself. Its local task is already outside
                // the queue and its unique_lock is unlocked. Do not let its
                // worker loop resume member access after that task returns.
                retireCommonPoolWorker = true;
                caller = &thread;
            } else if (thread.joinable()) {
                thread.join();
            }
        }
        if (caller) caller->detach();
    }
};

CommonPoolWorkers& commonPoolWorkers() {
    static CommonPoolWorkers workers;
    return workers;
}
} // namespace

CommonPool::~CommonPool() {
    {
        std::lock_guard lock(mLock);
        mIsStopping = true;
    }
    mCondition.notify_all();
    commonPoolWorkers().join();
}
`;

let cpp=exact(source,'CommonPool::CommonPool() : CommonPoolBase() {',owner+'\nCommonPool::CommonPool() : CommonPoolBase() {');
cpp=exact(cpp,'    // Create 2 workers\n','    auto& workers = commonPoolWorkers();\n    try {\n    // Create 2 workers\n');
cpp=exact(cpp,'        std::thread worker([pool, i, &mLock, &tids, &tidConditionVars] {',String.raw`#ifdef COMMON_POOL_TESTING
        commonPoolTestBeforeCreate(i);
#endif
        workers.threads[i] = std::thread([pool, i, &mLock, &tids, &tidConditionVars] {`);
cpp=exact(cpp,'        worker.detach();\n','');
cpp=exact(cpp,'            work();\n            lock.lock();','            work();\n            if (retireCommonPoolWorker) return;\n            lock.lock();');
cpp=exact(cpp,'    if (pool->supportsTid()) {\n        mWorkerThreadIds = std::move(tids);\n    }\n}',`    if (pool->supportsTid()) {
        mWorkerThreadIds = std::move(tids);
    }
    } catch (...) {
        // Setup callbacks still reference the local mutex/conditions above.
        // Stop and join every successfully created worker before those locals
        // or the partially constructed pool are destroyed.
        {
            std::lock_guard lock(pool->mLock);
            pool->mIsStopping = true;
        }
        pool->mCondition.notify_all();
        workers.join();
        throw;
    }
}`);
const h=exact(header,'    ~CommonPool() {\n        mIsStopping = true;\n        mCondition.notify_all();\n    }','    ~CommonPool();');

return {header:h,source:cpp};
}
export function prepareCommonPool({framework,output}) {
const original=fs.realpathSync(path.join(framework,'libs/hwui'));
output=path.resolve(output);
if(output===original||output.startsWith(original+path.sep)||original.startsWith(output+path.sep))throw Error('Adapted HWUI output overlaps original source');
try { if(fs.lstatSync(output).isSymbolicLink())throw Error('Adapted HWUI output must be an owned directory'); } catch(error) { if(error.code!=='ENOENT')throw error; }
fs.mkdirSync(output,{recursive:true});
const physical=fs.realpathSync(output);
if(physical===original||physical.startsWith(original+path.sep)||original.startsWith(physical+path.sep))throw Error('Adapted HWUI output aliases original source');
const checkOutput=dir=>{for(const name of fs.readdirSync(dir)){const file=path.join(dir,name),stat=fs.lstatSync(file);if(stat.isSymbolicLink())throw Error('Adapted HWUI output contains an alias');if(stat.isDirectory())checkOutput(file);}};
checkOutput(output);
fs.cpSync(original,output,{recursive:true,dereference:true});
const adapted=adaptCommonPool({header:fs.readFileSync(path.join(original,'thread/CommonPool.h'),'utf8'),source:fs.readFileSync(path.join(original,'thread/CommonPool.cpp'),'utf8')});
fs.writeFileSync(path.join(output,'thread/CommonPool.h'),adapted.header);fs.writeFileSync(path.join(output,'thread/CommonPool.cpp'),adapted.source);
return {output,files:['thread/CommonPool.h','thread/CommonPool.cpp']};
}
