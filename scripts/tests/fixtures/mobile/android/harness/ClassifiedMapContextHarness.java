import fixture.Core;
import fixture.Trace;
import java.util.*;
import java.util.function.Function;
@SuppressWarnings({"rawtypes","unchecked"})
public final class ClassifiedMapContextHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned classified map context mismatch: "+detail);
    }
    private static void attempt(final int route,final int mode) {
        final Object source=mode==14?null:new Object(),key=new Object(),marker=new Object(),lateMarker=new Object();
        final Collection values=mode==3?null:new ArrayList();
        final boolean flag=(mode&1)!=0,expectedFlag=route==1 || route==4 || route==6?false:flag;
        final Map original=new HashMap(),late=new HashMap(),tree=new TreeMap();final Object invalid=new Object();
        final RuntimeException runtime=new IllegalStateException("owned runtime");final Error error=new LinkageError("owned error");
        final int[] counts={0,0};final Object[] seen={null};
        final Function classifier=mode==1 || mode==2?null:value -> {
            counts[1]++;check(value==source && counts[1]==1 && Trace.stage==1,"original classifier argument/order");
            if(mode==6)throw runtime;if(mode==7)throw error;if(mode==18)Trace.result=late;
            return mode==15?null:key;
        };
        Trace.calls=Trace.stage=Trace.kind=0;Trace.values=null;Trace.classifier=null;Trace.flag=!expectedFlag;
        Trace.result=mode==11?tree:original;Trace.marker=marker;
        Trace.action=mode==19?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.kind==(route>=5?2:1)
                    && Trace.values==values && Trace.classifier==classifier && Trace.flag==expectedFlag,"exact provider and original incoming arguments");
            if(mode==4)throw runtime;if(mode==5)throw error;
            if(mode!=1)seen[0]=classifier.apply(source);
            if(mode==8)Trace.result=null;if(mode==9)Trace.result=late;if(mode==10)Trace.result=invalid;
            if(mode==12)Trace.marker=lateMarker;if(mode==13)Trace.classifier=null;
            if(mode==16 && values!=null)values.add(source);if(mode==17)Trace.flag=!expectedFlag;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(values,classifier,flag);break;
                case 1:result=Core.simple(values,classifier);break;
                case 2:result=Core.checked(values,classifier,flag);break;
                case 3:result=Core.ignored(values,classifier,flag);break;
                case 4:result=Core.relay(values,classifier);break;
                case 5:result=Core.value(values,classifier,flag);break;
                case 6:result=Core.valueRelay(values,classifier);break;
                default:throw new AssertionError("owned route");
            }
        } catch(Throwable caught){failure=caught;}
        int callbacks=mode==1 || mode==2 || mode==4 || mode==5 || mode==19?0:1;
        boolean actionFailed=mode==2 || mode==4 || mode==5 || mode==6 || mode==7 || mode==19;
        check(Trace.calls==1 && Trace.stage==(actionFailed?1:2) && counts[0]==(mode==19?0:1) && counts[1]==callbacks,"one invocation and original partial effects");
        if(mode==4 || mode==6)check(failure==runtime,"runtime identity");
        else if(mode==5 || mode==7)check(failure==error,"error identity");
        else if(mode==2 || mode==19)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==10 || mode==11 && route==2)check(failure instanceof ClassCastException,"original delayed map cast");
        else {
            check(failure==null && result==(route==3?mode==12?lateMarker:marker:mode==8?null:mode==9 || mode==18?late:mode==11?tree:original),"opaque result and late marker");
            check(seen[0]==(mode==1 || mode==15?null:key),"classifier result reference");
            if(mode==16)check(values.size()==1 && values.iterator().next()==source,"original collection mutation");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,classified-map-context=matched");
    }
}
