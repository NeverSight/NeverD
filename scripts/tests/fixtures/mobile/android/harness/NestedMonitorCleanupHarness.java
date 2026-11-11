import fixture.*;
public final class NestedMonitorCleanupHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned nested monitor cleanup");
    }
    static void raise(Throwable failure) {
        if (failure instanceof Error) throw (Error) failure;
        throw (RuntimeException) failure;
    }
    public static void main(String[] args) throws Throwable {
        MonitorCopyCleanupHarness.main(new String[0]);
        int cases=0;
        for(int route=0;route<4;route++)for(int stage=0;stage<3;stage++)
        for(int kind=0;kind<2;kind++)for(int mask=0;mask<16;mask++)for(int attempt=0;attempt<2;attempt++) {
            if(stage==2&&kind==1)continue;
            final int selected=stage,failures=mask;
            Gate<Object> gate=new Gate<>();Object lock=new Object();int[] trace={0},calls={0,0};
            Throwable original=kind==0?new IllegalArgumentException():new AssertionError();
            Throwable[] errors=new Throwable[5];
            for(int event=1;event<=4;event++)errors[event]=((event+kind+attempt)&1)==0?new IllegalStateException():new LinkageError();
            Cleanup.observer=event->{require(Thread.holdsLock(lock));trace[0]=trace[0]*10+event;
                if((failures&(1<<(event-1)))!=0)raise(errors[event]);};
            Payload.partial=null;
            Payload.observer=()->{require(Thread.holdsLock(lock));calls[0]++;if(selected==0)raise(original);};
            Runnable callback=()->{require(Thread.holdsLock(lock));calls[1]++;
                if(selected==1)raise(original);if(selected==2)gate.failure=(RuntimeException)original;};
            Throwable expected=original;
            if((mask&1)!=0)expected=errors[1];
            else {
                if((mask&2)!=0)expected=errors[2];
                if((mask&4)!=0)expected=errors[3];
            }
            if((mask&8)!=0)expected=errors[4];
            Throwable actual=null;
            try{MonitorCopyCleanupHarness.call(route,gate,lock,callback);}catch(Throwable failure){actual=failure;}
            require(actual==expected&&trace[0]==((mask&1)!=0?14:1234));
            require(calls[0]==1&&calls[1]==(stage==0?0:1));
            require(Payload.partial!=null&&Payload.partial.anchor==lock);
            require(gate.payload==(stage==0?null:Payload.partial)&&gate.count==(stage==2?1:0));
            require(!Thread.holdsLock(lock));MonitorRegionHarness.released(lock);
            Cleanup.observer=null;Payload.observer=null;cases++;
        }
        for(int route=0;route<4;route++) {
            final int path=route;Gate<Object> outer=new Gate<>(),inner=new Gate<>();Object lock=new Object();
            RuntimeException original=new IllegalArgumentException(),nested=new IllegalStateException();
            int[] depth={0},events={0,0};
            Cleanup.observer=event->{require(Thread.holdsLock(lock));events[depth[0]]++;
                if(depth[0]==0&&event==1) {
                    depth[0]=1;Throwable actual=null;
                    try{MonitorCopyCleanupHarness.call(path,inner,lock,()->raise(nested));}catch(Throwable failure){actual=failure;}
                    require(actual==nested&&Thread.holdsLock(lock));depth[0]=0;
                }};
            Throwable actual=null;
            try{MonitorCopyCleanupHarness.call(route,outer,lock,()->raise(original));}catch(Throwable failure){actual=failure;}
            require(actual==original&&events[0]==4&&events[1]==4&&!Thread.holdsLock(lock));
            MonitorRegionHarness.released(lock);Cleanup.observer=null;
        }
        System.out.println("nested-cleanup-cases="+cases+",nested-reentrant-cases=4");
    }
}
