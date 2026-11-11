import fixture.Core;
import fixture.Subject;
import fixture.Trace;
import java.util.Comparator;

public final class ScopedNullConstructorHarness {
    private static void check(boolean value,String detail) {
        if(!value)throw new AssertionError("owned scoped null constructor mismatch: "+detail);
    }
    private static Object call(int route,int number,Comparator<Object> callback) {
        switch(route) {
            case 0:return Core.simple(number);
            case 1:return Core.direct(number,callback);
            case 2:return Core.typed(number);
            case 3:return Core.ignored(number);
            case 4:return Core.directNull(number);
            case 5:return Core.other(number);
            default:throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route,final int mode,int attempt) {
        final int number=mode==1?0:mode==2?-1:mode==3?Integer.MIN_VALUE:mode==4?Integer.MAX_VALUE:37;
        final int[] actions={0},comparisons={0};
        final Comparator<Object> callback=mode==14?null:(a,b) -> {
            comparisons[0]++;throw new AssertionError("unexpected comparator execution");
        };
        final Comparator<Object> expected=route==1?callback:null;
        final Object selected=new Object(),late=new Object(),marker=new Object();
        final Object[] array=new Object[2];
        final RuntimeException runtime=new IllegalStateException("owned runtime");
        final Error error=new LinkageError("owned error");
        Trace.bases=Trace.calls=Trace.shortCalls=Trace.number=Trace.stage=0;
        Trace.owner=null;Trace.callback=null;Trace.value=selected;Trace.marker=marker;
        Trace.action=mode==5?null:() -> {
            actions[0]++;
            Subject<?> partial=(Subject<?>)Trace.owner;
            check(actions[0]==1 && Trace.bases==1 && Trace.calls==1 && Trace.shortCalls==0
                    && Trace.stage==1 && Trace.number==number && Trace.callback==expected
                    && partial.value==null && partial.number==0 && partial.callback==null,
                    "original target, null scope and partial fields");
            if(mode==6)throw runtime;
            if(mode==7)throw error;
            if(mode==8)Trace.value=null;
            if(mode==9)Trace.value=late;
            if(mode==10)Trace.number=-number;
            if(mode==11)Trace.callback=null;
            if(mode==12)Trace.marker=late;
            if(mode==13)Trace.action=() -> {throw new AssertionError("late action");};
            if(mode==15)Trace.value=array;
            if(mode==16)Trace.value=expected;
            if(mode==17){Trace.number=number+1;Trace.marker=null;}
        };
        Object result=null;Throwable failure=null;
        try{result=call(route,number,callback);}catch(Throwable caught){failure=caught;}
        boolean failed=mode==5 || mode==6 || mode==7;
        Subject<?> subject=(Subject<?>)Trace.owner;
        check(subject!=null && Trace.calls==1 && Trace.bases==1 && comparisons[0]==0
                && Trace.shortCalls==(failed || route==1 || route==4?0:1)
                && Trace.stage==(failed?1:2) && actions[0]==(mode==5?0:1),"one exact constructor chain");
        if(mode==5)check(failure instanceof NullPointerException,"null action after initialization");
        else if(mode==6 || mode==7)check(failure==(mode==6?runtime:error),"action failure identity");
        else {
            check(failure==null && subject.value==(mode==8?null:mode==9?late:mode==15?array:mode==16?expected:selected)
                    && subject.number==number && (Object)subject.callback==expected,"late value and original parameters");
            check(result==(route==3?mode==12?late:mode==17?null:marker:subject),"ignored result and late marker");
        }
        if(failed)check(subject.value==null && subject.number==0 && subject.callback==null,"failed target leaves default final fields");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++)attempt(route,mode,attempt);
        System.out.println("attempts=2,scoped-null-constructor=matched");
    }
}
