import fixture.Core;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class MapUnaryResultHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned map unary result mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Object key=mode==18?null:new Object(),value=mode==17?null:new Object(),marker=new Object(),lateMarker=new Object();
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] counts=new int[4];
        Iterator cursor=new Iterator() {
            public boolean hasNext(){throw new AssertionError("owned map unary result mismatch: eager hasNext");}
            public Object next(){counts[3]++;check(counts[3]==1 && Trace.stage==1 && counts[2]==1,"next order");if(mode==11)throw runtime;if(mode==12)throw error;return value;}
        };
        Iterable sequence=() -> {counts[2]++;check(counts[2]==1 && counts[1]==1 && Trace.stage==1,"iterator order");if(mode==9)throw runtime;if(mode==10)throw error;return cursor;};
        Map actual=new AbstractMap() {
            public Set entrySet(){throw new AssertionError("owned map unary result mismatch: unexpected entries");}
            public Object get(Object supplied){counts[1]++;check(counts[1]==1 && supplied==key && Trace.stage==1,"exact key and map-get order");if(mode==5)throw runtime;if(mode==6)throw error;return mode==7?new Object():mode==8?null:sequence;}
        };
        Map input=mode==1?null:actual;
        Trace.calls=Trace.stage=0;Trace.input=null;Trace.result=new ArrayList();Trace.marker=marker;Trace.built=Trace.inner=null;
        Trace.action=mode==2?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.input==input,"exact provider input and order");
            if(mode==3)throw runtime;if(mode==4)throw error;
            Iterable nested=(Iterable)input.get(key);Object selected=nested.iterator().next();Map inner=new HashMap();inner.put(key,selected);ArrayList built=new ArrayList();built.add(inner);if(mode==19)built.add(inner);Trace.inner=inner;Trace.built=built;Trace.result=built;
            if(mode==13)Trace.result=null;if(mode==14)Trace.result=new Object();if(mode==15){Trace.result=new LinkedList(built);Trace.built=Trace.result;}if(mode==16)Trace.marker=lateMarker;
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
        int gets=action==0 || mode==1 || mode==3 || mode==4?0:1;
        int iter=gets==0 || mode==5 || mode==6 || mode==7 || mode==8?0:1;
        int next=iter==0 || mode==9 || mode==10?0:1;
        boolean actionFailed=mode>=1 && mode<=12;
        check(Trace.calls==1 && Trace.input==input && Trace.stage==(actionFailed?1:2)
                && counts[0]==action && counts[1]==gets && counts[2]==iter && counts[3]==next,"exact original partial effects");
        if(mode==1 || mode==2 || mode==8)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==3 || mode==5 || mode==9 || mode==11)check(failure==runtime,"runtime identity");
        else if(mode==4 || mode==6 || mode==10 || mode==12)check(failure==error,"error identity");
        else if(mode==7 || mode==14 || mode==15 && route==1)check(failure instanceof ClassCastException,"original nested/provider/delayed result cast");
        else check(failure==null && result==(route==2?mode==16?lateMarker:marker:mode==13?null:Trace.built),"opaque returned list and late marker identity");
        if(!actionFailed) {
            List built=(List)Trace.built;Map inner=(Map)Trace.inner;check(built.size()==(mode==19?2:1) && built.get(0)==inner && inner.size()==1 && inner.get(key)==value && (mode!=19 || built.get(1)==inner),"nested pair value and map aliases");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,map-unary-result=matched");
    }
}
