import fixture.Core;
import fixture.Subject;
import fixture.Trace;

public final class ObjectArrayConstructorHarness {
    private static void check(boolean value,String detail) {
        if(!value)throw new AssertionError("owned object array constructor mismatch: "+detail);
    }
    private static Object call(int route,Object input) {
        switch(route) {
            case 0:return Core.array((Object[])input);
            case 1:return Core.strings((String[])input);
            case 2:return Core.direct(input);
            case 3:return Core.ignored((Object[])input);
            case 4:return Core.nullValue();
            case 5:return new Subject<Object>((Object[])input);
            default:throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route,final int mode,int attempt) {
        final Object input=mode==1?null:route==2 && mode==3?new Object()
                :route==1 && mode!=16?new String[mode==2?0:2]:new Object[mode==2?0:2];
        final Object expected=route==4?null:input,late=new Object(),marker=new Object();
        final Object[] lateArray=new Object[2];
        final String replacement=new String("owned replacement");
        final RuntimeException runtime=new IllegalStateException("owned runtime");
        final Error error=new LinkageError("owned error");
        final int[] actions={0};
        Trace.calls=Trace.arrays=Trace.objects=Trace.stage=Trace.tag=0;
        Trace.value=late;Trace.marker=marker;
        Trace.action=mode==4?null:() -> {
            actions[0]++;
            check(actions[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.tag==23
                    && Trace.value==expected && Trace.arrays==0 && Trace.objects==0,"selected target and original input");
            if(mode==12 || mode==17) {
                if(expected instanceof Object[] && ((Object[])expected).length>0)((Object[])expected)[0]=replacement;
            }
            if(mode==5 || mode==17)throw runtime;
            if(mode==6)throw error;
            if(mode==7)Trace.value=null;
            if(mode==8)Trace.value=late;
            if(mode==9)Trace.value=lateArray;
            if(mode==10)Trace.value=expected;
            if(mode==11)Trace.marker=late;
            if(mode==13)Trace.marker=null;
            if(mode==14)Trace.tag=-1;
            if(mode==15)Trace.action=() -> {throw new AssertionError("late action");};
        };
        Object result=null;Throwable failure=null;
        try{result=call(route,input);}catch(Throwable caught){failure=caught;}
        if(route==1 && mode==16) {
            check(failure instanceof ClassCastException && actions[0]==0 && Trace.calls==0
                    && Trace.stage==0 && Trace.arrays==0 && Trace.objects==0,"source cast before constructor");return;
        }
        boolean failed=mode==4 || mode==5 || mode==6 || mode==17;
        if(mode==4)check(failure instanceof NullPointerException,"null action");
        else if(mode==5 || mode==17)check(failure==runtime,"runtime identity");
        else if(mode==6)check(failure==error,"error identity");
        else {
            check(failure==null,"construction completed");
            if(route==3)check(result==(mode==11?late:mode==13?null:marker),"ignored result and late marker");
            else {
                check(result instanceof Subject,"original subject result");
                Subject<?> subject=(Subject<?>)result;
                check(subject.value==(mode==7?null:mode==8?late:mode==9?lateArray:expected)
                        && subject.tag==23,"late field value and original tag");
            }
        }
        check(Trace.calls==1 && Trace.arrays==(failed || route==2?0:1)
                && Trace.objects==(failed?0:1) && Trace.stage==(failed?1:2)
                && actions[0]==(mode==4?0:1),"one exact constructor chain");
        if((mode==12 || mode==17) && expected instanceof Object[] && ((Object[])expected).length>0)
            check(((Object[])expected)[0]==replacement,"original array identity");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++)attempt(route,mode,attempt);
        System.out.println("attempts=2,object-array-constructor=matched");
    }
}
