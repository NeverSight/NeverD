import fixture.*;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class CollectionPrimitiveResultHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned collection primitive result mismatch: "+detail);
    }
    private static void attempt(final int route,final int mode) {
        final int[] counts={0,0,0};
        final Object token=new Object();
        final RuntimeException runtime=new IllegalStateException("owned runtime");
        final Error error=new LinkageError("owned error");
        final List source=mode==1?null:new AbstractList() {
            public Object get(int index) {counts[2]++;throw new AssertionError("owned unexpected get");}
            public int size() {counts[2]++;throw new AssertionError("owned unexpected size");}
        };
        final Test test=mode==16?null:new Test() {
            public boolean accept(Object value) {
                counts[1]++;check(value==token,"unchanged callback argument");
                if(mode==17)throw runtime;return true;
            }
        };
        final int[] primary=mode==8?new int[0]:new int[]{13,-5,42};
        final int[] late=new int[]{77,-11};
        final Object initial=mode==2?null:mode==6?new Object():mode==7?new long[]{1}:primary;
        final Object expectedResult=mode==9?late:mode==10?null:initial;
        final int index=mode==11?-1:mode==12?3:mode==13?Integer.MAX_VALUE:mode==18?1:0;
        final int value=mode==19?Integer.MIN_VALUE:-27;
        final Object marker=new Object(),lateMarker=new Object();
        Trace.calls=Trace.stage=0;Trace.source=null;Trace.test=null;
        Trace.result=initial;Trace.marker=marker;Trace.index=index;Trace.value=value;
        Trace.action=mode==3?null:() -> {
            counts[0]++;
            check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.source==source && Trace.test==test,"exact provider and context order");
            if(mode==4)throw runtime;if(mode==5)throw error;
            if(mode==9)Trace.result=late;if(mode==10)Trace.result=null;
            if(mode==14)Trace.marker=lateMarker;if(mode==15)primary[0]=93;
            if(mode==17)test.accept(token);
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(source,test);break;
                case 1:result=Core.from(source,test);break;
                case 2:result=Core.element(source,test);break;
                case 3:result=Core.ignored(source,test);break;
                case 4:result=Core.relay(source,test);break;
                case 5:result=Core.write(source,test);break;
                default:throw new AssertionError("owned route");
            }
        } catch(Throwable caught) {failure=caught;}
        boolean actionFailed=mode==3||mode==4||mode==5||mode==17;
        check(Trace.calls==1 && Trace.stage==(actionFailed?1:2) && counts[0]==(mode==3?0:1)
                && Trace.source==source && Trace.test==test && counts[1]==(mode==17?1:0) && counts[2]==0,"exact partial effects and original inputs");
        if(mode==3)check(failure instanceof NullPointerException,"original action null failure");
        else if(mode==4||mode==17)check(failure==runtime,"runtime and callback failure identity");
        else if(mode==5)check(failure==error,"error identity");
        else if(mode==6||mode==7)check(failure instanceof ClassCastException,"original primitive array cast");
        else if((route==2||route==5) && expectedResult==null)check(failure instanceof NullPointerException,"delayed null array access");
        else if((route==2||route==5) && (index<0||index>=((int[])expectedResult).length))check(failure instanceof ArrayIndexOutOfBoundsException,"original array boundary");
        else if(route==2) {
            int wanted=mode==9?77:mode==15?93:index==1?-5:13;
            check(failure==null && result instanceof Integer && ((Integer)result).intValue()==wanted,"unchanged original array load");
        } else {
            Object wanted=route==3?mode==14?lateMarker:marker:expectedResult;
            check(failure==null && result==wanted,"opaque array or late marker identity");
            if(route==5)check(((int[])expectedResult)[index]==value,"original post-call array write");
        }
        if(mode==15 && route!=5)check(primary[0]==93,"partial original array mutation");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        attempt(route,mode);attempt(route,mode);
        System.out.println("attempts=2,collection-primitive-result=matched");
    }
}
