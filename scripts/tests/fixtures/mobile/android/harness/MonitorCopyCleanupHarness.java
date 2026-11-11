import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class MonitorCopyCleanupHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned monitor copy cleanup");}
    static void call(int route,Gate<?> gate,Object lock,Runnable callback) throws Throwable {
        if(route==0)gate.update(lock,callback);
        else if(route==1)MonitorRegionCaller.update(gate,lock,callback);
        else if(route==2)try{Gate.class.getMethod("update",Object.class,Runnable.class).invoke(gate,lock,callback);}catch(InvocationTargetException e){throw e.getCause();}
        else MethodHandles.publicLookup().findVirtual(Gate.class,"update",MethodType.methodType(void.class,Object.class,Runnable.class)).invokeExact((Gate)gate,lock,callback);
    }
    static void raise(Throwable failure){if(failure instanceof Error)throw(Error)failure;throw(RuntimeException)failure;}
    public static void main(String[] args) throws Throwable {
        MonitorRegionHarness.main(new String[0]);
        int failures=0,reentrant=0;
        for(int route=0;route<4;route++) {
            for(int stage=0;stage<3;stage++)for(int kind=0;kind<2;kind++) {
                if(stage==2&&kind==1)continue;
                final int selected=stage;
                for(int attempt=0;attempt<2;attempt++) {
                    Gate<Object> gate=new Gate<>();Object lock=new Object();int[] events={0,0};
                    Throwable expected=kind==0?new IllegalArgumentException("owned original"):new AssertionError("owned original error");
                    Payload.partial=null;Payload.observer=()->{require(Thread.holdsLock(lock));events[0]++;if(selected==0)raise(expected);};
                    Runnable callback=()->{require(Thread.holdsLock(lock));events[1]++;if(selected==1)raise(expected);if(selected==2)gate.failure=(RuntimeException)expected;};
                    Throwable actual=null;try{call(route,gate,lock,callback);}catch(Throwable failure){actual=failure;}
                    require(actual==expected&&!Thread.holdsLock(lock));
                    require(events[0]==1&&events[1]==(stage==0?0:1));
                    require(Payload.partial!=null&&Payload.partial.anchor==lock);
                    require(gate.payload==(stage==0?null:Payload.partial)&&gate.count==(stage==2?1:0));
                    MonitorRegionHarness.released(lock);Payload.observer=null;failures++;
                }
            }
            Gate<Object> gate=new Gate<>();Object lock=new Object();
            RuntimeException expected=new IllegalStateException("owned nested failure");final int path=route;
            call(route,gate,lock,()->{require(Thread.holdsLock(lock));Throwable actual=null;
                try{call(path,gate,lock,()->raise(expected));}catch(Throwable failure){actual=failure;}
                require(actual==expected&&Thread.holdsLock(lock));});
            require(gate.count==1&&!Thread.holdsLock(lock));MonitorRegionHarness.released(lock);reentrant++;
        }
        System.out.println("split-cleanup-failures="+failures+",exception-and-error-identity="+failures+",reentrant-release-checks="+reentrant);
    }
}
