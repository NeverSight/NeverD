import fixture.Core;
import fixture.Trace;
import java.util.*;
import java.util.function.Function;
import java.util.stream.Collector;
@SuppressWarnings({"rawtypes","unchecked"})
public final class GroupedMapContextHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned grouped map context mismatch: "+detail);
    }
    private static void attempt(final int route,final int mode) {
        final Object source=mode==16?null:new Object(),key=new Object(),finished=new Object(),stateMarker=new Object(),marker=new Object(),lateMarker=new Object();
        final Collection values=mode==17?null:new ArrayList();
        final boolean flag=(mode&1)!=0,expectedFlag=route==1 || route==4?false:flag;
        final Map original=new HashMap(),late=new HashMap(),tree=new TreeMap();
        final Object invalid=new Object();final Object[] seen=new Object[3];final int[] counts=new int[5];
        final RuntimeException runtime=new IllegalStateException("owned runtime");final Error error=new LinkageError("owned error");
        final Function classifier=mode==1 || mode==2?null:value -> {
            counts[1]++;check(value==source && counts[1]==1 && Trace.stage==1,"classifier source/order");
            if(mode==6)throw runtime;if(mode==7)throw error;return key;
        };
        final Collector target=mode==1 || mode==3?null:Collector.of(
            () -> {counts[2]++;check(counts[1]==1 && counts[2]==1,"supplier order");if(mode==8)throw runtime;return new Object[]{stateMarker,null};},
            (Object[] state,Object value) -> {counts[3]++;check(state==seen[1] && state[0]==stateMarker && value==source && counts[3]==1,"original state and accumulator input");if(mode==9)throw error;state[1]=value;},
            (Object[] left,Object[] right) -> {throw new AssertionError("unexpected combiner");},
            (Object[] state) -> {counts[4]++;check(state==seen[1] && state[1]==source && counts[3]==1 && counts[4]==1,"original finisher state");if(mode==10)throw runtime;return finished;});
        Trace.calls=Trace.stage=0;Trace.values=null;Trace.classifier=null;Trace.downstream=null;Trace.flag=!expectedFlag;
        Trace.result=mode==14?tree:original;Trace.marker=marker;
        Trace.action=mode==19?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.values==values
                    && Trace.classifier==classifier && Trace.downstream==target && Trace.flag==expectedFlag,"complete original incoming arguments");
            if(mode==4)throw runtime;if(mode==5)throw error;
            if(mode!=1) {
                seen[0]=classifier.apply(source);
                seen[1]=target.supplier().get();
                target.accumulator().accept(seen[1],source);
                seen[2]=target.finisher().apply(seen[1]);
            }
            if(mode==11)Trace.result=null;if(mode==12)Trace.result=late;if(mode==13)Trace.result=invalid;
            if(mode==15)Trace.marker=lateMarker;
            if(mode==18){Trace.values=null;Trace.classifier=null;Trace.downstream=null;}
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(values,classifier,target,flag);break;
                case 1:result=Core.simple(values,classifier,target);break;
                case 2:result=Core.checked(values,classifier,target,flag);break;
                case 3:result=Core.ignored(values,classifier,target,flag);break;
                case 4:result=Core.relay(values,classifier,target);break;
                default:throw new AssertionError("owned route");
            }
        } catch(Throwable caught){failure=caught;}
        int fn=mode==1 || mode==2 || mode==4 || mode==5 || mode==19?0:1;
        int supply=fn==0 || mode==3 || mode==6 || mode==7?0:1;
        int accumulate=supply==0 || mode==8?0:1;
        int finish=accumulate==0 || mode==9?0:1;
        boolean actionFailed=mode>=2 && mode<=10 || mode==19;
        check(Trace.calls==1 && Trace.stage==(actionFailed?1:2) && counts[0]==(mode==19?0:1)
                && counts[1]==fn && counts[2]==supply && counts[3]==accumulate && counts[4]==finish,"one invocation and original partial effects");
        if(mode==4 || mode==6 || mode==8 || mode==10)check(failure==runtime,"runtime identity");
        else if(mode==5 || mode==7 || mode==9)check(failure==error,"error identity");
        else if(mode==2 || mode==3 || mode==19)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==13 || mode==14 && route==2)check(failure instanceof ClassCastException,"original delayed map cast");
        else {
            check(failure==null && result==(route==3?mode==15?lateMarker:marker:mode==11?null:mode==12?late:mode==14?tree:original),"opaque result and late marker");
            if(mode!=1)check(seen[0]==key && seen[2]==finished,"classifier and downstream result references");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,grouped-map-context=matched");
    }
}
