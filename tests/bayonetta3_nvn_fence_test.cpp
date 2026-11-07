// Exercise the production queue ticket path directly, with a deferred worker
// and explicit GPU completion gate. Unused window/present paths are discarded.
#include "../runtime/src/nvn/nvn.h"
#undef NX_HLE
#define NX_HLE(sym, fn)
#include "../runtime/src/nvn/nvn_queue.cpp"
#include "../runtime/src/nvn/nvn_replay.cpp"
#include <deque>
#include <future>
#include <stdexcept>
#include <thread>
using namespace nx;
using namespace nx::nvn;
namespace {
std::deque<std::function<void()>> jobs;
std::map<uint64_t,std::shared_ptr<CmdList>> lists;
std::vector<std::unique_ptr<Object>> retired;
std::mutex gate_lock;
std::condition_variable gate_cv;
bool gpu_entered=false,gpu_released=false;
uint64_t submits=0,completions=0;
std::shared_ptr<const CommandListTable> last_captured;
std::mutex pending_lock;
std::deque<std::vector<std::function<void()>>> pending_completions;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void initialize(uint64_t address){CPU c{};c.x[0]=address;SyncInitialize(c);check(c.x[0]==1,"sync initialization");}
void submit(uint64_t handle){CPU c{};c.x[1]=1;c.x[2]=reinterpret_cast<uint64_t>(&handle);QueueSubmitCommands(c);}
std::shared_ptr<CmdList> fenced(uint64_t sync){auto l=std::make_shared<CmdList>();Cmd c{Cmd::FenceSync};c.a[0]=sync;l->cmds.push_back(c);return l;}
void run_one(){auto fn=std::move(jobs.front());jobs.pop_front();fn();}
void release_gpu(){std::lock_guard lock(gate_lock);gpu_released=true;gate_cv.notify_all();}
void reset_gate(){std::lock_guard lock(gate_lock);gpu_entered=gpu_released=false;}
}
namespace nx {
std::atomic<uint64_t> g_frame{0};
void log(const char*,...){}
[[noreturn]] void fatal(const char*,...){throw std::runtime_error("production fatal");}
}
namespace nx::plat {
uint64_t monotonic_ns(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
int64_t current_thread_id(){return 1;}
void thread_counters(uint64_t& cpu,long* faults,long* switches){cpu=monotonic_ns();faults[0]=faults[1]=switches[0]=switches[1]=0;}
void perf_hint_thread(){}
}
namespace nx::nvn {
ObjectsLock& objects_lock(){static ObjectsLock l;return l;}
std::unordered_map<uint64_t,std::unique_ptr<Object>>& objects(){static std::unordered_map<uint64_t,std::unique_ptr<Object>> o;return o;}
bool resolve_cache_on(){return false;}
Object* find_object(uint64_t g,uint64_t){auto i=objects().find(g);return i==objects().end()?nullptr:i->second.get();}
void defer_object(std::unique_ptr<Object> o){retired.push_back(std::move(o));}
std::shared_ptr<CmdList> command_list(uint64_t h){auto i=lists.find(h);return i==lists.end()?nullptr:i->second;}
DeferredValues deferred_snapshot(const std::vector<std::shared_ptr<CmdList>>&,std::shared_ptr<const CommandListTable>){return {};}
bool pipelined(){return false;}  // NX_FENCE_AT_CMD off: these tests pin the completion at the end of the submit
uint64_t worker_run(std::function<void()> fn){jobs.push_back(std::move(fn));return jobs.size();}
void worker_wait(uint64_t serial){check(serial==0,"command fence wait must not cycle on its worker job");}
bool gpu_wait(uint64_t serial){check(serial==0,"command fence wait has actual completion ticket");return false;}
uint64_t gpu_serial(){return 0;}
void gpu_submit(const std::vector<std::shared_ptr<CmdList>>&,std::shared_ptr<const DeferredValues>,
                std::shared_ptr<const CommandListTable> captured,std::vector<std::function<void()>> callbacks){
    submits++;last_captured=std::move(captured);
    if(!callbacks.empty()) { std::lock_guard lock(pending_lock); pending_completions.push_back(std::move(callbacks)); }
}
void gpu_finish_fenced_submit(){
    std::unique_lock lock(gate_lock);gpu_entered=true;gate_cv.notify_all();
    gate_cv.wait(lock,[]{return gpu_released;});completions++;
}
uint32_t frame_replays_in_flight(){std::lock_guard lock(pending_lock);return uint32_t(pending_completions.size());}
bool frame_retire_one(){
    std::vector<std::function<void()>> callbacks;
    { std::lock_guard lock(pending_lock);
      if(pending_completions.empty())return false;
      callbacks=std::move(pending_completions.front());pending_completions.pop_front(); }
    gpu_finish_fenced_submit();
    for(auto& callback:callbacks)callback();
    return true;
}
}
int main(int argc,char** argv){
try{
    unsetenv("NX_SYNC_LOOSE");unsetenv("NX_DEFERRED_SNAPSHOT");
    unsetenv("NX_FAST_SYNC");unsetenv("NX_SYNC_AHEAD");
    const bool deferred=argc>1 && std::string(argv[1])=="deferred";
    if(deferred)unsetenv("NX_SYNC_FENCE_DEFER");else setenv("NX_SYNC_FENCE_DEFER","0",1);
    constexpr uint64_t sync=0x1000;
    initialize(sync);lists[1]=fenced(sync);submit(1);
    if(deferred){
        auto ticket=get<Sync>(sync)->command_fence.load();
        check(ticket && !ticket->complete && submits==0,"deferred ticket armed before worker");
        reset_gate();run_one();
        check(submits==1 && !ticket->complete && frame_replays_in_flight()==1,
              "deferred submit returns before real GPU completion");
        auto waiter=std::async(std::launch::async,[&]{CPU c{};c.x[0]=sync;SyncWait(c);return c.x[0];});
        {std::unique_lock lock(gate_lock);check(gate_cv.wait_for(lock,std::chrono::seconds(1),[]{return gpu_entered;}),
                                                  "host waiter drives retirement");}
        check(waiter.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout && !ticket->complete,
              "deferred ticket waits for GPU completion");
        release_gpu();
        check(waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready && waiter.get()==0,
              "deferred ticket completes after retirement");
        check(ticket->complete && completions==1,"retirement publishes deferred completion");
        puts("PASS: deferred fence completes only on GPU retirement");
        return 0;
    }
    auto first=get<Sync>(sync)->command_fence.load();
    check(first && !first->complete && submits==0,"ticket armed before deferred worker executes");
    submit(1);auto second=get<Sync>(sync)->command_fence.load();
    check(second && second!=first && !second->complete,"repeated submit creates distinct ticket");
    reset_gate();std::thread old_worker(run_one);
    {std::unique_lock lock(gate_lock);check(gate_cv.wait_for(lock,std::chrono::seconds(1),[]{return gpu_entered;}),"worker reaches actual GPU completion wait");}
    check(!first->complete && !second->complete,"gpu_submit return alone does not complete any ticket");
    release_gpu();old_worker.join();
    check(first->complete && !second->complete && get<Sync>(sync)->command_fence.load()==second,"old completion cannot signal newer generation");
    // Wait for the newest ticket through the production SyncWait. Completing
    // the old ticket must not make the host wait pass.
    std::promise<void> started;auto ready=started.get_future();
    auto waiter=std::async(std::launch::async,[&]{started.set_value();CPU c{};c.x[0]=sync;SyncWait(c);return c.x[0];});
    ready.wait();check(waiter.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"host wait blocks on newest generation");
    reset_gate();std::thread new_worker(run_one);
    {std::unique_lock lock(gate_lock);check(gate_cv.wait_for(lock,std::chrono::seconds(1),[]{return gpu_entered;}),"second worker enters GPU completion gate");}
    check(waiter.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,"host wait remains blocked until whole-submit GPU finish");
    release_gpu();new_worker.join();
    check(waiter.wait_for(std::chrono::seconds(1))==std::future_status::ready && waiter.get()==0,"host wait wakes after real completion");
    check(completions==2 && second->complete,"both real completions published");
    if(extra_wait_timing()){
        check(g_ticket_calls==1 && g_ticket_pending==1 && g_ticket_ns>0,"ticket timing measures the actual blocked wait");
        check(g_embedded_tickets==2 && g_embedded_submits==2,"embedded fence totals count containing submits");
    }
    // Reinitializing the guest address replaces the Sync, but the captured
    // ticket remains valid and cannot publish completion into the replacement.
    submit(1);auto third=get<Sync>(sync)->command_fence.load();initialize(sync);
    check(!get<Sync>(sync)->command_fence.load(),"reinitialization has no stale ticket");
    release_gpu();run_one();
    check(third->complete && !get<Sync>(sync)->command_fence.load(),"old ticket completion leaves replacement untouched");
    // A nested captured list is armed before replay too.
    lists[2]=fenced(sync);lists[3]=std::make_shared<CmdList>();Cmd call{Cmd::CallList};call.a[0]=2;lists[3]->cmds.push_back(call);
    submit(3);auto nested=get<Sync>(sync)->command_fence.load();check(nested && !nested->complete,"nested fence armed synchronously");
    auto old_child=lists[2];initialize(0x2000);lists[2]=fenced(0x2000);
    release_gpu();run_one();check(nested->complete,"nested ticket completes");
    check(last_captured && last_captured->at(2)==old_child,"same frozen child graph passed from ticket arming to GPU replay");
    check(!get<Sync>(0x2000)->command_fence.load(),"later child rerecord cannot arm an unrelated sync");
    // Production lookup honors captured absence and original child identity,
    // even if a handle appears or is replaced before worker replay.
    submit_lists_capture(last_captured);check(submit_command_list(2)==old_child,"production replay uses frozen child");
    lists[99]=fenced(0x2000);auto absent=capture_command_lists({77});lists[77]=fenced(0x2000);
    submit_lists_capture(absent);check(!submit_command_list(77),"captured missing handle cannot appear during replay");
    submit_lists_capture({});
    // Old QueueFence worker completes its own ticket while leaving the current
    // command generation and reinitialized guest object untouched.
    CPU q{};q.x[1]=sync;QueueFenceSync(q);auto old_queue=get<Sync>(sync)->command_fence.load();
    submit(1);auto later_command=get<Sync>(sync)->command_fence.load();
    check(old_queue && later_command && old_queue!=later_command,"queue and command fence generations distinct");
    run_one();check(old_queue->complete && !later_command->complete && get<Sync>(sync)->command_fence.load()==later_command,"old queue completion leaves later command armed");
    release_gpu();run_one();check(later_command->complete,"later command completes independently");
    QueueFenceSync(q);auto before_reinit=get<Sync>(sync)->command_fence.load();initialize(sync);run_one();
    check(before_reinit->complete && !get<Sync>(sync)->command_fence.load(),"old queue callback cannot mutate replacement Sync");
    QueueFenceSync(q);auto queue_a=get<Sync>(sync)->command_fence.load();QueueFenceSync(q);auto queue_b=get<Sync>(sync)->command_fence.load();
    run_one();check(queue_a->complete && !queue_b->complete && get<Sync>(sync)->command_fence.load()==queue_b,"old queue callback cannot replace newer queue ticket");
    run_one();check(queue_b->complete,"latest queue ticket completes");
    puts("PASS: production fence immediate arm, real completion gate, host wait, reinitialize, frozen nested lists and queue/command generations");
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
