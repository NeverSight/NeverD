import fixture.Core;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class IterableMapResultHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned iterable map result mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Object key=mode==18?null:new Object(),otherKey=new Object(),value=mode==17?null:new Object(),marker=new Object(),lateMarker=new Object();
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] counts=new int[4];
        Map row=new AbstractMap() {
            public Set entrySet(){throw new AssertionError("owned iterable map result mismatch: unexpected entry iteration");}
            public Object get(Object supplied){counts[3]++;check(counts[3]==1 && supplied==key && Trace.stage==1 && counts[2]==1,"exact map key and get order");if(mode==11)throw runtime;if(mode==12)throw error;return value;}
        };
        Iterator cursor=new Iterator() {
            public boolean hasNext(){throw new AssertionError("owned iterable map result mismatch: eager hasNext");}
            public Object next(){counts[2]++;check(counts[2]==1 && Trace.stage==1,"next order");if(mode==7)throw runtime;if(mode==8)throw error;return mode==9?new Object():mode==10?null:row;}
        };
        Iterable actual=() -> {counts[1]++;check(counts[1]==1 && Trace.stage==1,"iterator order");if(mode==5)throw runtime;if(mode==6)throw error;return cursor;};
        Iterable input=mode==1?null:actual;
        Trace.calls=Trace.stage=0;Trace.input=null;Trace.key=key;Trace.result=new HashMap();Trace.marker=marker;Trace.built=Trace.inner=null;
        Trace.action=mode==2?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.input==input,"exact provider input and order");
            if(mode==3)throw runtime;if(mode==4)throw error;
            Map item=(Map)input.iterator().next();Object selected=item.get(Trace.key);ArrayList inner=new ArrayList();inner.add(selected);HashMap built=new HashMap();built.put(Trace.key,inner);if(mode==19)built.put(otherKey,inner);Trace.inner=inner;Trace.built=built;Trace.result=built;
            if(mode==13)Trace.result=null;if(mode==14)Trace.result=new Object();if(mode==15){Trace.result=new IdentityHashMap(built);Trace.built=Trace.result;}if(mode==16)Trace.marker=lateMarker;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(input);break;
                case 1:result=Core.checked(input);break;
                case 2:result=Core.ignored(input);break;
                case 3:result=Core.relay(input);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        int action=mode==2?0:1;
        int iter=action==0 || mode==1 || mode==3 || mode==4?0:1;
        int next=iter==0 || mode==5 || mode==6?0:1;
        int gets=next==0 || mode==7 || mode==8 || mode==9 || mode==10?0:1;
        boolean actionFailed=mode>=1 && mode<=12;
        check(Trace.calls==1 && Trace.input==input && Trace.key==key && Trace.stage==(actionFailed?1:2)
                && counts[0]==action && counts[1]==iter && counts[2]==next && counts[3]==gets,"exact original partial effects");
        if(mode==1 || mode==2 || mode==10)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==3 || mode==5 || mode==7 || mode==11)check(failure==runtime,"runtime identity");
        else if(mode==4 || mode==6 || mode==8 || mode==12)check(failure==error,"error identity");
        else if(mode==9 || mode==14 || mode==15 && route==1)check(failure instanceof ClassCastException,"original map/provider/delayed result cast");
        else check(failure==null && result==(route==2?mode==16?lateMarker:marker:mode==13?null:Trace.built),"opaque returned map or late marker identity");
        if(!actionFailed) {
            Map built=(Map)Trace.built;check(built.size()==(mode==19?2:1) && built.get(key)==Trace.inner && ((List)Trace.inner).size()==1 && ((List)Trace.inner).get(0)==value && (mode!=19 || built.get(otherKey)==Trace.inner),"nested value and list aliases");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,iterable-map-result=matched");
    }
}
