import fixture.*;
public final class UnlockedMonitorLoopsHarness {
    static void require(boolean value) {
        if (!value) throw new AssertionError("owned unlocked monitor loops");
    }
    static void raise(Throwable failure) {
        if (failure instanceof Error) throw (Error) failure;
        throw (RuntimeException) failure;
    }
    public static void main(String[] args) throws Throwable {
        MonitorCopyCleanupHarness.main(new String[0]);
        int cases=0, nulls=0, callbacks=0, reentrant=0;
        for (int route=0; route<4; route++) {
            for (int lead : new int[]{-2,0,1,3}) for (int tail : new int[]{-2,0,1,3}) {
                final int a=Math.max(lead,0), b=Math.max(tail,0);
                for (int stage=-1; stage<5; stage++) for (int kind=0; kind<2; kind++) {
                    if ((stage==-1 || stage==3) && kind==1 || stage==0 && a==0 || stage==4 && b==0) continue;
                    final int selected=stage;
                    for (int attempt=0; attempt<2; attempt++) {
                        Gate<Object> gate=new Gate<>(); Object lock=new Object();
                        gate.leadSteps=lead; gate.tailSteps=tail;
                        Throwable expected=stage==-1 ? null : kind==0 ?
                            new IllegalArgumentException("owned loop failure") : new AssertionError("owned loop error");
                        int[] events={0,0}; Payload.partial=null;
                        Payload.observer=()->{
                            require(Thread.holdsLock(lock) && gate.leadCount==a && gate.count==0);
                            events[1]++; if (selected==1) raise(expected);
                        };
                        Runnable callback=()->{
                            int at=events[0]++;
                            if (at<a) {
                                require(!Thread.holdsLock(lock) && gate.leadCount==at && gate.payload==null && gate.count==0);
                                if (selected==0) raise(expected);
                            } else if (at==a) {
                                require(Thread.holdsLock(lock) && gate.leadCount==a && gate.payload==Payload.partial && gate.count==0);
                                if (selected==2) raise(expected);
                                if (selected==3) gate.failure=(RuntimeException)expected;
                                gate.leadSteps=73; gate.tailSteps=91;
                            } else {
                                require(!Thread.holdsLock(lock) && gate.count==1 && gate.tailCount==at-a-1 && at<a+b+1);
                                if (selected==4) raise(expected);
                            }
                        };
                        Throwable actual=null;
                        try { MonitorCopyCleanupHarness.call(route,gate,lock,callback); } catch (Throwable failure) { actual=failure; }
                        require(actual==expected && !Thread.holdsLock(lock));
                        require(gate.leadCount==(stage==0 ? 0 : a) && gate.tailCount==(stage==-1 ? b : 0));
                        require(gate.count==(stage==-1 || stage==3 || stage==4 ? 1 : 0));
                        require(events[1]==(stage==0 ? 0 : 1));
                        require(events[0]==(stage==0 ? 1 : stage==1 ? a : stage==4 ? a+2 : stage==-1 ? a+b+1 : a+1));
                        require(gate.payload==(stage==0 || stage==1 ? null : Payload.partial));
                        if (stage!=0) require(Payload.partial!=null && Payload.partial.anchor==lock);
                        if (lead==0 && tail==0 && attempt==0) MonitorRegionHarness.released(lock);
                        Payload.observer=null; cases++;
                    }
                }
                Gate<Object> noCallback=new Gate<>(); Object lock=new Object();
                noCallback.leadSteps=lead; noCallback.tailSteps=tail;
                MonitorCopyCleanupHarness.call(route,noCallback,lock,null);
                require(noCallback.leadCount==a && noCallback.tailCount==b && noCallback.count==1 && noCallback.payload.anchor==lock);
                require(!Thread.holdsLock(lock)); callbacks++;
            }
            for (int lead : new int[]{0,2}) for (int tail : new int[]{0,3}) {
                Gate<Object> gate=new Gate<>(); gate.leadSteps=lead; gate.tailSteps=tail; int[] seen={0};
                Throwable actual=null;
                try { MonitorCopyCleanupHarness.call(route,gate,null,()->{
                    require(gate.count==0 && gate.payload==null && gate.leadCount==seen[0]); seen[0]++;
                }); } catch (Throwable failure) { actual=failure; }
                require(actual instanceof NullPointerException && gate.leadCount==lead && seen[0]==lead && gate.tailCount==0 && gate.count==0 && gate.payload==null);
                nulls++;
            }
            Gate<Object> gate=new Gate<>(); Object lock=new Object(); int[] seen={0}; final int path=route;
            gate.leadSteps=1; gate.tailSteps=1;
            MonitorCopyCleanupHarness.call(route,gate,lock,()->{
                int at=seen[0]++;
                require(Thread.holdsLock(lock)==(at==1));
                if (at==0) {
                    gate.leadSteps=0; gate.tailSteps=0;
                    try { MonitorCopyCleanupHarness.call(path,gate,lock,()->require(Thread.holdsLock(lock))); }
                    catch (Throwable failure) { throw new AssertionError(failure); }
                }
            });
            require(seen[0]==3 && gate.leadCount==1 && gate.tailCount==1 && gate.count==2 && !Thread.holdsLock(lock));
            MonitorRegionHarness.released(lock); reentrant++;
        }
        require(cases==1024 && nulls==16 && callbacks==64 && reentrant==4);
        System.out.println("unlocked-loop-cases="+cases+",unlocked-null-acquisitions="+nulls+",unlocked-null-callbacks="+callbacks+",unlocked-reentrant-cases="+reentrant);
    }
}
