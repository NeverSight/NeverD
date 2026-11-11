import fixture.Core;
import fixture.Trace;
import java.util.*;
import java.util.function.Function;
public final class KeyedMapCarrierHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned keyed map carrier mismatch: "+detail);
    }
    @SuppressWarnings("unchecked")
    private static Object call(int route,Collection values,Function first,Function second) {
        switch(route) {
            case 0:return Core.flat(values,first,second);
            case 1:return Core.list(values,first,second);
            case 2:return Core.nested(values,first,second);
            case 3:return Core.nestedList(values,first,second);
            case 4:return Core.checked(values,first,second);
            case 5:return Core.ignored(values,first,second);
            default:throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route,final int mode) {
        final Object source=new Object(),lateSource=new Object(),left=new Object(),right=new Object(),marker=new Object(),lateMarker=new Object();
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");
        final Error error=new LinkageError("owned error");
        final Collection<Object> values=mode==4?null:new ArrayList<Object>();
        final int[] counts={0,0,0};
        final Function<Object,Object> first=mode==2?null:value -> {
            counts[0]++;
            check(value==source && Trace.stage==1 && counts[0]==1 && counts[1]==0,"first callback input/order");
            if(mode==5)throw runtime;
            if(mode==14)Trace.source=lateSource;
            return mode==16?null:left;
        };
        final Function<Object,Object> second=mode==3?null:value -> {
            counts[1]++;
            check(value==source && Trace.stage==2 && counts[0]==1 && counts[1]==1,"second callback input/order");
            if(mode==6)throw error;
            return mode==17?null:right;
        };
        final Map<Object,Object> initial=new HashMap<Object,Object>(),late=new HashMap<Object,Object>(),tree=new TreeMap<Object,Object>();
        Trace.values=null;Trace.first=null;Trace.second=null;Trace.targets=Trace.stage=0;
        Trace.source=source;Trace.seen=Trace.left=Trace.right=null;Trace.invoke=mode!=1;Trace.ordered=true;
        Trace.result=mode==18?tree:initial;Trace.marker=marker;
        Trace.action=mode==9?null:() -> {
            counts[2]++;
            check(counts[2]==1 && Trace.targets==1 && Trace.values==values && Trace.first==first && Trace.second==second
                    && !Trace.ordered && Trace.seen==source && Trace.stage==(mode==1?1:3)
                    && Trace.left==(mode==1 || mode==16?null:left) && Trace.right==(mode==1 || mode==17?null:right),"complete captured inputs and callback results");
            if(mode==7)throw runtime;
            if(mode==8)throw error;
            if(mode==10)Trace.result=null;
            if(mode==11)Trace.result=source;
            if(mode==12)Trace.result=late;
            if(mode==13)Trace.marker=lateMarker;
            if(mode==15){Trace.first=null;Trace.second=null;}
            if(mode==19)Trace.action=null;
        };
        Object result=null;Throwable failure=null;
        try{result=call(route,values,first,second);}catch(Throwable caught){failure=caught;}
        boolean callbackFailed=mode==2 || mode==3 || mode==5 || mode==6;
        int stage=mode==2 || mode==5?1:mode==3 || mode==6?2:mode==7 || mode==8 || mode==9?3:4;
        check(Trace.targets==1 && Trace.stage==stage && counts[0]==(mode==1 || mode==2?0:1)
                && counts[1]==(mode==1 || mode==2 || mode==3 || mode==5?0:1)
                && counts[2]==(callbackFailed || mode==9?0:1),"one provider invocation and exact partial effects");
        if(mode==5 || mode==7)check(failure==runtime,"runtime identity");
        else if(mode==6 || mode==8)check(failure==error,"error identity");
        else if(mode==2 || mode==3 || mode==9)check(failure instanceof NullPointerException,"null callback/action stage");
        else if(mode==11 || (mode==18 && route==4))check(failure instanceof ClassCastException,"original delayed result cast");
        else check(failure==null && result==(route==5?mode==13?lateMarker:marker:mode==10?null:mode==12?late:mode==18?tree:initial),"opaque result identity and late marker");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,keyed-map-carrier=matched");
    }
}
