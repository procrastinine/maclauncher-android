package maclauncher.android.framework;
import android.app.job.*;
import android.app.JobSchedulerImpl;
import android.content.pm.ParceledListSlice;
import android.os.*;
import android.app.Application;
import android.app.Service;
import android.content.pm.PackageManager;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import java.util.*;

/** Application-lifetime jobs; no system daemon or persistent execution promise. */
public final class HostJobScheduler extends IJobScheduler.Default {
    public static final int RESULT_FAILURE=0,RESULT_SUCCESS=1;
    private static volatile Owner host;
    private final Owner owner;
    private HostJobScheduler(Owner owner){this.owner=owner;}
    public static synchronized void hostConfigure(Application application,Looper looper){
        if(host!=null)throw new IllegalStateException("Job scheduler is already configured");
        if(!looper.isCurrentThread())throw new IllegalStateException("Job scheduler requires its application owner");
        host=new Owner(Objects.requireNonNull(application),looper);
    }
    public static JobScheduler hostInstance(){Owner value=host;if(value==null)throw new IllegalStateException("Application job scheduler is not attached");return value.client;}
    public static void hostShutdown(){Owner value=host;if(value!=null)value.shutdown();}
    public static void hostCancelService(android.content.ComponentName service){Owner value=host;if(value!=null)synchronized(value){value.jobs.entrySet().removeIf(e->e.getValue().job.getService().equals(service));value.wake();}}
    @Override public int schedule(String namespace,JobInfo job){return owner.schedule(namespace,Objects.requireNonNull(job));}
    @Override public int enqueue(String namespace,JobInfo job,JobWorkItem work){Objects.requireNonNull(work);return owner.enqueue(namespace,job,work);}
    @Override public int scheduleAsPackage(String namespace,JobInfo job,String packageName,int user,String tag){if(!owner.application.getPackageName().equals(packageName)||user!=android.os.Process.myUserHandle().getIdentifier())throw new SecurityException("Job belongs to another application");return schedule(namespace,job);}
    @Override public void cancel(String namespace,int id){owner.cancel(namespace,id,false);}
    @Override public void cancelAllInNamespace(String namespace){owner.cancel(namespace,0,true);}
    @Override public void cancelAll(){synchronized(owner){owner.jobs.clear();owner.wake();}}
    @Override public ParceledListSlice<JobInfo> getAllPendingJobsInNamespace(String namespace){synchronized(owner){var list=new ArrayList<JobInfo>();for(Entry e:owner.jobs.values())if(Objects.equals(namespace,e.namespace))list.add(e.job);return new ParceledListSlice<>(list);}}
    @Override public JobInfo getPendingJob(String namespace,int id){synchronized(owner){Entry e=owner.jobs.get(new Key(namespace,id));return e==null?null:e.job;}}
    @Override public Map<String,ParceledListSlice<JobInfo>> getAllPendingJobs(){synchronized(owner){var groups=new LinkedHashMap<String,List<JobInfo>>();for(Entry e:owner.jobs.values())groups.computeIfAbsent(e.namespace,k->new ArrayList<>()).add(e.job);var result=new LinkedHashMap<String,ParceledListSlice<JobInfo>>();groups.forEach((name,jobs)->result.put(name,new ParceledListSlice<>(jobs)));return result;}}
    @Override public boolean canRunUserInitiatedJobs(String packageName){return false;}
    @Override public boolean hasRunUserInitiatedJobsPermission(String packageName,int user){return false;}
    @Override public List<JobInfo> getStartedJobs(){synchronized(owner){return owner.active==null?List.of():List.of(owner.active.job);}}
    @Override public ParceledListSlice<JobSnapshot> getAllJobSnapshots(){return ParceledListSlice.emptyList();}
    @Override public List<PendingJobReasonsInfo> getPendingJobReasonsHistory(String namespace,int id){return List.of();}
    @Override public int getPendingJobReason(String namespace,int id){synchronized(owner){Entry e=owner.jobs.get(new Key(namespace,id));return e==null?JobScheduler.PENDING_JOB_REASON_INVALID_JOB_ID:e==owner.active?JobScheduler.PENDING_JOB_REASON_EXECUTING:JobScheduler.PENDING_JOB_REASON_CONSTRAINT_MINIMUM_LATENCY;}}
    @Override public int[] getPendingJobReasons(String namespace,int id){return new int[]{getPendingJobReason(namespace,id)};}
    private record Key(String namespace,int id){}
    static final class Entry {
        final String namespace;final JobInfo job;final Key key;final long deadline;
        long due,started;int failures,stopAction,nextWorkId;JobService service;IJobService endpoint;IJobCallback callback;JobParameters parameters;boolean completed,retry,stopping;
        final ArrayDeque<JobWorkItem> pendingWork=new ArrayDeque<>();final Map<Integer,JobWorkItem> executingWork=new LinkedHashMap<>();
        Entry(String namespace,JobInfo job,long now){this.namespace=namespace;this.job=job;key=new Key(namespace,job.getId());due=after(now,job.isPeriodic()?job.getIntervalMillis():job.getMinLatencyMillis());deadline=after(now,job.getMaxExecutionDelayMillis());if(job.hasLateConstraint())due=Math.min(due,deadline);}
    }
    private static long after(long now,long delay){return delay>Long.MAX_VALUE-now?Long.MAX_VALUE:now+delay;}
    static final class Owner {
        private static final int LIMIT=128;private static final long EXECUTION_LIMIT=600000;
        final Application application;final Looper looper;final Handler handler;final HostJobScheduler root;final JobScheduler client;
        final LinkedHashMap<Key,Entry> jobs=new LinkedHashMap<>();final Runnable pump=this::pump;
        final Runnable closing=this::shutdown;
        Entry active;boolean queued,closed;long wakeAt;
        Owner(Application application,Looper looper){this.application=application;this.looper=looper;handler=new Handler(looper);root=new HostJobScheduler(this);client=new JobSchedulerImpl(application,root);HostServices.register("jobscheduler",root,"android.app.job.IJobScheduler");if(!maclauncher.android.framework.HostLooper.addShutdownListener(looper,closing))throw new IllegalStateException("Application Looper is closed");}
        int schedule(String namespace,JobInfo job){
            if(!application.getPackageName().equals(job.getService().getPackageName()))throw new IllegalArgumentException("Job service belongs to another application");
            android.content.pm.ServiceInfo declared;
            try{declared=application.getPackageManager().getServiceInfo(job.getService(),0);}
            catch(PackageManager.NameNotFoundException missing){throw new IllegalArgumentException("Job service is not declared",missing);}
            if(!JobService.PERMISSION_BIND.equals(declared.permission))throw new IllegalArgumentException("Job service requires BIND_JOB_SERVICE");
            // These constraints need operating-system facilities not supplied by
            // this in-process owner. Never retain a request that cannot run.
            if(!declared.enabled||declared.processName!=null&&!declared.processName.equals(HostContext.hostProcessName())||job.isPersisted()||job.getRequiredNetwork()!=null||job.isRequireCharging()||job.isRequireDeviceIdle()||job.isRequireBatteryNotLow()||job.isRequireStorageNotLow()||job.isUserInitiated()||job.getTriggerContentUris()!=null)return RESULT_FAILURE;
            synchronized(this){
                if(closed)return RESULT_FAILURE;Key key=new Key(namespace,job.getId());
                if(!jobs.containsKey(key)&&jobs.size()>=LIMIT)return RESULT_FAILURE;
                Entry previous=jobs.put(key,new Entry(namespace,job,SystemClock.uptimeMillis()));
                try{if(wake())return RESULT_SUCCESS;}
                catch(RuntimeException|Error error){if(previous==null)jobs.remove(key);else jobs.put(key,previous);throw error;}
                if(previous==null)jobs.remove(key);else jobs.put(key,previous);return RESULT_FAILURE;
            }
        }
        int enqueue(String namespace,JobInfo job,JobWorkItem work){
            synchronized(this){Entry entry=jobs.get(new Key(namespace,job.getId()));if(entry==null){if(schedule(namespace,job)!=RESULT_SUCCESS)return RESULT_FAILURE;entry=jobs.get(new Key(namespace,job.getId()));}if(entry.pendingWork.size()+entry.executingWork.size()>=128)return RESULT_FAILURE;work.setWorkId(++entry.nextWorkId);entry.pendingWork.addLast(work);wake();return RESULT_SUCCESS;}
        }
        void cancel(String namespace,int id,boolean all){synchronized(this){jobs.entrySet().removeIf(e->Objects.equals(namespace,e.getKey().namespace())&&(all||id==e.getKey().id()));wake();}}
        // At most one queued owner task, regardless of caller rate or job count.
        boolean wake(){return arm(SystemClock.uptimeMillis());}
        boolean arm(long when){
            if(closed)return false;if(queued&&wakeAt<=when)return true;
            if(queued){handler.removeCallbacks(pump);queued=false;}
            long now=SystemClock.uptimeMillis();wakeAt=Math.max(now,Math.min(when,after(now,86400000L)));
            queued=handler.postAtTime(pump,wakeAt);return queued;
        }
        void finished(Entry e,boolean retry){synchronized(this){
            if(active!=e||e.completed)return;
            e.completed=true;e.retry=retry;wake();
        }}
        final class Callback extends IJobCallback.Default {
            final Entry entry;final Binder binder=new Binder("android.app.job.IJobCallback");
            Callback(Entry entry){this.entry=entry;binder.attachInterface(this,"android.app.job.IJobCallback");}
            @Override public IBinder asBinder(){return binder;}
            private boolean valid(int id){return id==entry.job.getId()&&active==entry&&entry.callback==this;}
            @Override public void acknowledgeStartMessage(int id,boolean ongoing){synchronized(Owner.this){if(valid(id)&&!ongoing)finished(entry,false);}}
            @Override public void acknowledgeStopMessage(int id,boolean retry){synchronized(Owner.this){if(valid(id))finished(entry,retry);}}
            @Override public void jobFinished(int id,boolean retry){synchronized(Owner.this){if(valid(id))finished(entry,retry);}}
            @Override public void handleAbandonedJob(int id){synchronized(Owner.this){if(valid(id))finished(entry,true);}}
            @Override public JobWorkItem dequeueWork(int id){synchronized(Owner.this){if(!valid(id))return null;JobWorkItem item=entry.pendingWork.pollFirst();if(item!=null){item.bumpDeliveryCount();entry.executingWork.put(item.getWorkId(),item);}return item;}}
            @Override public boolean completeWork(int id,int work){synchronized(Owner.this){return valid(id)&&entry.executingWork.remove(work)!=null;}}
            @Override public void setNotification(int id,int notificationId,android.app.Notification notification,int policy){synchronized(Owner.this){if(!valid(id))return;}application.getSystemService(android.app.NotificationManager.class).notify(notificationId,notification);}
        }
        private void checkOwner(){if(!looper.isCurrentThread())throw new IllegalStateException("Job callbacks require the application main thread");}
        void pump(){
            checkOwner();synchronized(this){queued=false;}
            for(int i=0;i<LIMIT;i++){
                Entry e;int action;long now=SystemClock.uptimeMillis();
                synchronized(this){
                    if(closed)return;e=active;action=0;
                    if(e!=null){if(e.completed)action=2;else if(!e.stopping&&jobs.get(e.key)!=e)action=1;else if(!e.stopping&&now-e.started>=EXECUTION_LIMIT)action=3;}
                    else{for(Entry candidate:jobs.values())if(candidate.due<=now&&(e==null||candidate.job.getPriority()>e.job.getPriority()))e=candidate;
                        if(e!=null){active=e;e.started=now;action=4;}}
                }
                if(action==0)break;
                if(action==4)start(e);else if(action==2)retire(e,e.stopAction);else stop(e,action);
            }
            synchronized(this){
                if(closed)return;long next=Long.MAX_VALUE;
                if(active!=null)next=active.completed?SystemClock.uptimeMillis():active.stopping?Long.MAX_VALUE:after(active.started,EXECUTION_LIMIT);
                else for(Entry e:jobs.values())next=Math.min(next,e.due);
                if(next!=Long.MAX_VALUE&&!arm(next))throw new IllegalStateException("Application Looper refused job scheduling");
            }
        }
        private void start(Entry e){
            Throwable failure=null;
            try{
                Service component=HostApplication.instantiateService(e.job.getService().getClassName(),new android.content.Intent().setComponent(e.job.getService()));
                if(!(component instanceof JobService service))throw new IllegalArgumentException("Declared job service does not extend JobService");
                synchronized(this){e.service=service;e.callback=new Callback(e);e.parameters=new JobParameters(e.callback.asBinder(),e.namespace,e.job.getId(),e.job.getExtras(),e.job.getTransientExtras(),e.job.getClipData(),e.job.getClipGrantFlags(),e.job.hasLateConstraint()&&SystemClock.uptimeMillis()>=e.deadline,e.job.isExpedited(),e.job.isUserInitiated(),null,null,null);}
                HostApplication.attachService(service,application,new Binder(),e.job.getService().getClassName());service.onCreate();
                e.endpoint=IJobService.Stub.asInterface(service.onBind(new android.content.Intent()));
                e.endpoint.startJob(e.parameters);
                return;
            }catch(Throwable error){failure=error instanceof java.lang.reflect.InvocationTargetException wrapped?wrapped.getCause():error;}
            synchronized(this){jobs.remove(e.key,e);active=null;}
            if(e.service!=null)try{e.service.onDestroy();}catch(Throwable error){if(error!=failure)failure.addSuppressed(error);}
            if(e.service!=null)try{e.service.detachAndCleanUp();}catch(Throwable error){if(error!=failure)failure.addSuppressed(error);}
            e.service=null;e.parameters=null;e.callback=null;rethrow(failure);
        }
        private void stop(Entry entry,int action){
            entry.stopping=true;entry.stopAction=action;
            entry.parameters.setStopReason(action==1?JobParameters.STOP_REASON_CANCELLED_BY_APP:JobParameters.STOP_REASON_TIMEOUT,action==1?JobParameters.INTERNAL_STOP_REASON_CANCELED:JobParameters.INTERNAL_STOP_REASON_TIMEOUT,"Application job stopped");
            try{entry.endpoint.stopJob(entry.parameters);}catch(RemoteException failure){finished(entry,false);}
        }
        private void retire(Entry e,int action){
            boolean retry=e.retry;Throwable failure=null;
            try{e.service.onDestroy();}catch(Throwable error){if(failure==null)failure=error;else if(error!=failure)failure.addSuppressed(error);}
            try{e.service.detachAndCleanUp();}catch(Throwable error){if(failure==null)failure=error;else if(error!=failure)failure.addSuppressed(error);}
            synchronized(this){
                active=null;e.service=null;e.endpoint=null;e.callback=null;e.parameters=null;e.completed=false;e.stopping=false;e.stopAction=0;
                if(retry){for(JobWorkItem item:e.executingWork.values())e.pendingWork.addFirst(item);}e.executingWork.clear();
                if(jobs.get(e.key)==e){
                    if(failure!=null||action==1)jobs.remove(e.key);
                    else if(retry){e.failures=Math.min(30,e.failures+1);long factor=e.job.getBackoffPolicy()==JobInfo.BACKOFF_POLICY_LINEAR?e.failures:1L<<(e.failures-1);long delay=factor>JobInfo.MAX_BACKOFF_DELAY_MILLIS/e.job.getInitialBackoffMillis()?JobInfo.MAX_BACKOFF_DELAY_MILLIS:factor*e.job.getInitialBackoffMillis();e.due=after(SystemClock.uptimeMillis(),delay);}
                    else if(e.job.isPeriodic()){e.failures=0;e.due=after(SystemClock.uptimeMillis(),e.job.getIntervalMillis());}
                    else jobs.remove(e.key);
                }
            }
            if(failure!=null)rethrow(failure);
        }
        void shutdown(){
            checkOwner();Entry e;synchronized(this){if(closed)return;closed=true;handler.removeCallbacks(pump);maclauncher.android.framework.HostLooper.removeShutdownListener(looper,closing);queued=false;jobs.clear();e=active;}
            Throwable failure=null;
            if(e!=null){try{if(!e.completed&&!e.stopping)stop(e,1);HostLooper.drain(looper);}catch(Throwable error){failure=error;}try{retire(e,1);}catch(Throwable error){if(failure==null)failure=error;else if(error!=failure)failure.addSuppressed(error);}}
            HostServices.unregister("jobscheduler",root,"android.app.job.IJobScheduler");host=null;
            if(failure!=null)rethrow(failure);
        }
        private static void rethrow(Throwable error){if(error instanceof RuntimeException v)throw v;if(error instanceof Error v)throw v;throw new IllegalStateException("Android job service failed",error);}
    }
}
