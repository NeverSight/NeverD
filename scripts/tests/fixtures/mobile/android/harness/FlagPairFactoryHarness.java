import fixture.*;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class FlagPairFactoryHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned flag pair factory mismatch: "+detail);
    }
    private static void attempt(final int route,final int mode) {
        final boolean flag=(mode&1)!=0;
        final int capacity=mode==15?Integer.MIN_VALUE:mode==16?Integer.MAX_VALUE:mode==17?-1:mode==18?0:7;
        final int expectedCapacity=route==1?capacity:73;
        final Object key=mode==11?new Object():new String("key");
        final Object value=mode==12?new Object():new String("value");
        final String lateValue=new String("late");
        final HashMap primary=mode==8?new LinkedHashMap():new HashMap();
        if(mode!=13)primary.put(key,value);
        final HashMap late=new LinkedHashMap();late.put(key,lateValue);
        final Object initial=mode==2?null:mode==6?new Object():mode==7?new TreeMap():primary;
        final Object expectedResult=mode==9?late:mode==10?null:initial;
        final Object expectedValue=mode==9||mode==19?lateValue:value;
        final Object marker=new Object(),lateMarker=new Object();
        final RuntimeException runtime=new IllegalStateException("owned runtime");
        final Error error=new LinkageError("owned error");
        final int[] counts={0};
        Trace.calls=Trace.stage=0;Trace.flag=flag;Trace.flagSeen=!flag;
        Trace.capacity=capacity;Trace.capacitySeen=expectedCapacity^123;
        Trace.result=initial;Trace.marker=marker;Trace.key=key;
        Trace.action=mode==3?null:() -> {
            counts[0]++;
            check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.flagSeen==flag && Trace.capacitySeen==expectedCapacity,"exact primitive snapshots and provider order");
            Trace.flag=!flag;Trace.capacity=capacity+13;
            if(mode==4)throw runtime;if(mode==5)throw error;
            if(mode==9)Trace.result=late;if(mode==10)Trace.result=null;
            if(mode==14)Trace.marker=lateMarker;if(mode==19)primary.put(key,lateValue);
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(flag);break;
                case 1:result=Core.capacity(capacity,flag);break;
                case 2:result=Core.checked(flag);break;
                case 3:result=Core.ignored(flag);break;
                case 4:result=Core.relay(flag);break;
                case 5:result=Core.key(flag);break;
                case 6:result=Core.value(flag);break;
                default:throw new AssertionError("owned route");
            }
        } catch(Throwable caught) {failure=caught;}
        boolean actionFailed=mode==3||mode==4||mode==5;
        check(Trace.calls==1 && Trace.stage==(actionFailed?1:2) && counts[0]==(mode==3?0:1)
                && Trace.flagSeen==flag && Trace.capacitySeen==expectedCapacity,"exact original partial effects and scalar identity");
        if(mode==3)check(failure instanceof NullPointerException,"original action null failure");
        else if(mode==4)check(failure==runtime,"runtime identity");
        else if(mode==5)check(failure==error,"error identity");
        else if(mode==6||mode==7)check(failure instanceof ClassCastException,"original provider HashMap cast");
        else if(route==2 && expectedResult!=null && !(expectedResult instanceof LinkedHashMap))check(failure instanceof ClassCastException,"delayed subtype cast");
        else if((route==5||route==6) && expectedResult==null)check(failure instanceof NullPointerException,"delayed result null failure");
        else if(route==5 && mode==13)check(failure instanceof NoSuchElementException,"empty key iterator timing");
        else if(route==5 && mode==11||route==6 && mode==12)check(failure instanceof ClassCastException,"delayed original element cast");
        else {
            Object wanted=route==3?mode==14?lateMarker:marker:route==5?key:route==6?mode==13?null:expectedValue:expectedResult;
            check(failure==null && result==wanted,"opaque result, exact key/value or late marker identity");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        attempt(route,mode);attempt(route,mode);
        System.out.println("attempts=2,flag-pair-factory=matched");
    }
}
