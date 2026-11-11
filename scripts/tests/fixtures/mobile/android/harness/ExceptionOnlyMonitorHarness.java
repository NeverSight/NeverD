import fixture.Core;
import fixture.Probe;
import fixture.Trace;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.management.ManagementFactory;
import java.lang.management.ThreadInfo;
import java.lang.management.ThreadMXBean;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Modifier;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

public final class ExceptionOnlyMonitorHarness {
    private static final ThreadMXBean THREADS=ManagementFactory.getThreadMXBean();
    private static volatile int calls;
    private static volatile Object payload;
    private static void check(boolean value) {
        if (!value) throw new AssertionError("owned exceptional monitor mismatch");
    }
    private static void invoke(int route,int kind,Core core,Object lock) throws Throwable {
        if (route==0) {if (kind==0) core.fail(); else Core.failLock(lock);}
        else if (route==1) ExceptionOnlyMonitorCaller.call(core,lock,kind);
        else if (route==2) {
            try {
                if (kind==0) Core.class.getMethod("fail").invoke(core);
                else Core.class.getMethod("failLock",Object.class).invoke(null,lock);
            } catch (InvocationTargetException failure) {throw failure.getCause();}
        } else if (kind==0)
            MethodHandles.publicLookup().findVirtual(Core.class,"fail",MethodType.methodType(void.class)).invokeExact(core);
        else
            MethodHandles.publicLookup().findStatic(Core.class,"failLock",MethodType.methodType(void.class,Object.class)).invokeExact(lock);
    }
    private static final class Worker extends Thread {
        final int route,kind;
        final Core core;
        final Object lock;
        final CountDownLatch ready=new CountDownLatch(1);
        volatile boolean finished;
        Throwable failure;
        Worker(int route,int kind,Core core,Object lock) {
            this.route=route;this.kind=kind;this.core=core;this.lock=lock;
            setDaemon(true);
        }
        public void run() {
            ready.countDown();
            try {invoke(route,kind,core,lock);}
            catch (Throwable caught) {failure=caught;}
            finally {finished=true;}
        }
    }
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        check(Modifier.isSynchronized(Core.class.getMethod("fail").getModifiers()));
        check(!Modifier.isSynchronized(Core.class.getMethod("failLock",Object.class).getModifiers()));
        check(Arrays.equals(Core.class.getMethod("fail").getExceptionTypes(),new Class[]{IOException.class}));
        check(Arrays.equals(Core.class.getMethod("failLock",Object.class).getExceptionTypes(),new Class[]{IOException.class}));
        Throwable previous=null;
        for (int attempt=0;attempt<2;attempt++) {
            final Core core=mode==4?null:new Core();
            final Object lock=mode==4?null:new Object();
            final Object held=kind==0?core:lock;
            final Object sentinel=new Object(),written=new Object();
            final IOException checked=new IOException("owned callback failure");
            final RuntimeException runtime=new IllegalStateException("owned callback runtime");
            final Error error=new Error("owned callback error");
            calls=0;payload=sentinel;
            final Probe replacement=new Probe() {public void observe(Object value) {throw new AssertionError("owned exceptional monitor mismatch repeated callback");}};
            Trace.probe=mode==7?null:new Probe() {public void observe(Object value) throws IOException {
                check(value==held&&Thread.holdsLock(held)&&calls==0&&payload==sentinel);
                calls++;payload=written;
                if (mode==6) Trace.probe=replacement;
                if (mode==1) throw checked;
                if (mode==2) throw runtime;
                if (mode==3) throw error;
            }};
            Worker worker=new Worker(route,kind,core,lock);
            if (held==null) worker.run();
            else if (mode==5) {
                synchronized (held) {worker.run();check(Thread.holdsLock(held));}
                check(!Thread.holdsLock(held));
            } else {
                synchronized (held) {
                    worker.start();check(worker.ready.await(3,TimeUnit.SECONDS));
                    boolean blocked=false;
                    long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(3);
                    while (!worker.finished&&System.nanoTime()<deadline) {
                        ThreadInfo info=THREADS.getThreadInfo(worker.getId());
                        if (info!=null&&info.getThreadState()==Thread.State.BLOCKED
                                &&info.getLockOwnerId()==Thread.currentThread().getId()
                                &&info.getLockInfo()!=null
                                &&info.getLockInfo().getIdentityHashCode()==System.identityHashCode(held)) {
                            blocked=true;break;
                        }
                        Thread.yield();
                    }
                    check(blocked&&calls==0&&payload==sentinel);
                }
                worker.join(3000);check(worker.finished&&!worker.isAlive());
            }
            check(calls==(mode==4||mode==7?0:1));
            check(payload==(mode==4||mode==7?sentinel:written));
            if (mode==1) check(worker.failure==checked);
            else if (mode==2) check(worker.failure==runtime);
            else if (mode==3) check(worker.failure==error);
            else if (mode==4||mode==7) check(worker.failure instanceof NullPointerException);
            else {
                check(worker.failure!=null&&worker.failure.getClass()==IOException.class
                    &&"owned monitor failure".equals(worker.failure.getMessage())&&worker.failure!=previous);
            }
            if (held!=null) check(!Thread.holdsLock(held));
            if (mode==6) check(Trace.probe==replacement);
            previous=worker.failure;
        }
        System.out.println("attempts=2,exception-only-monitor=matched");
    }
}
