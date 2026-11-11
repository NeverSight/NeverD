import fixture.Base;
import fixture.Child;
import fixture.Direct;
import fixture.Residue;
import fixture.Trace;
import java.util.AbstractList;
import java.util.List;

public final class GuardedIntegerDivisionHarness {
    private static void check(boolean value,String detail) {
        if(!value)throw new AssertionError("owned guarded integer division mismatch: "+detail);
    }
    private static final class State {
        int calls,actions,first,second,divisor;
        final RuntimeException runtime=new IllegalStateException("owned runtime");
        final RuntimeException invalid=new IllegalArgumentException("owned invalid");
        final Error error=new LinkageError("owned error");
    }
    private static final class Values extends AbstractList<Object> {
        final State s;final int mode;
        Values(State s,int mode){this.s=s;this.mode=mode;}
        public Object get(int index){throw new AssertionError("unexpected traversal");}
        public int size() {
            s.calls++;
            if(s.calls==1) {
                check(Trace.calls==0 && Trace.owner==null && Trace.stage==0,"original first size before initialization");
                if(mode==6)throw s.runtime;
                if(mode==7)throw s.error;
                return s.first;
            }
            check(s.calls==2 && Trace.calls==1 && Trace.stage==2 && s.actions==1,"late size after superclass");
            if(mode==11)throw s.runtime;
            if(mode==12)throw s.error;
            return s.second;
        }
    }
    private static int divisor(Object value) {
        return value instanceof Child?((Child<?>)value).divisor
                :value instanceof Direct?((Direct<?>)value).divisor:((Residue<?>)value).divisor;
    }
    private static int remainder(Object value) {
        return value instanceof Child?((Child<?>)value).remainder
                :value instanceof Direct?((Direct<?>)value).remainder:((Residue<?>)value).remainder;
    }
    private static Object call(int route,List<Object> values,int divisor) {
        switch(route) {
            case 0:return new Child<Object>(values,divisor);
            case 1:return new Direct<Object>(values,divisor);
            case 2:return new Residue<Object>(values,divisor);
            case 3:new Child<Object>(values,divisor);return Trace.marker;
            case 4:new Direct<Object>(values,divisor);return Trace.marker;
            case 5:new Residue<Object>(values,divisor);return Trace.marker;
            default:throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route,final int mode,int attempt) {
        final State s=new State();final int kind=route%3;
        s.divisor=mode==1?0:mode==2?-1:mode==3?Integer.MIN_VALUE
                :mode==4?Integer.MAX_VALUE:mode==16?7:mode==17?-3:3;
        s.first=mode==2 || mode==3 || mode==4 || mode==16?Integer.MIN_VALUE:mode==15?0:14;
        s.second=-17;
        final int late=mode==2 || mode==4?Integer.MIN_VALUE:mode==3 || mode==16?Integer.MAX_VALUE:23;
        final List<Object> values=mode==5?null:new Values(s,mode);
        final Object marker=new Object(),changed=new Object();
        Trace.calls=Trace.checks=Trace.stage=0;Trace.owner=null;Trace.condition=false;
        Trace.marker=marker;Trace.failure=s.invalid;
        Trace.action=mode==8?null:() -> {
            s.actions++;
            Base<?> base=(Base<?>)Trace.owner;
            int expected=kind==2?s.first%s.divisor:s.first/(kind==0 && s.divisor<=0?1:s.divisor);
            check(s.actions==1 && s.calls==1 && Trace.calls==1 && Trace.stage==1
                    && base.list==values && base.result==expected
                    && divisor(base)==0 && remainder(base)==0,"original parent operands and partial fields");
            s.second=late;
            if(mode==13)Trace.marker=changed;
            if(mode==14)Trace.action=() -> {throw new AssertionError("late action");};
            if(mode==9)throw s.runtime;
            if(mode==10)throw s.error;
        };
        Object result=null;Throwable failure=null;
        try{result=call(route,values,s.divisor);}catch(Throwable caught){failure=caught;}
        boolean before=mode==5 || mode==6 || mode==7 || kind!=0 && s.divisor==0;
        boolean parentFailure=mode==8 || mode==9 || mode==10;
        boolean invalid=kind==0 && s.divisor<=0;
        if(before) {
            if(mode==5)check(failure instanceof NullPointerException && s.calls==0,"null list before arithmetic");
            else if(mode==6 || mode==7)check(failure==(mode==6?s.runtime:s.error) && s.calls==1,"first size failure identity");
            else check(failure instanceof ArithmeticException && s.calls==1,"zero divisor after size");
            check(Trace.owner==null && Trace.calls==0 && Trace.stage==0 && s.actions==0 && Trace.checks==0,"no initialization after prefix failure");return;
        }
        Base<?> base=(Base<?>)Trace.owner;
        check(base!=null && base.list==values && Trace.calls==1 && s.calls==(parentFailure || invalid?1:2)
                && Trace.checks==(kind==0 && !parentFailure?1:0),"one original initialization and suffix order");
        if(parentFailure) {
            check(mode==8?failure instanceof NullPointerException:failure==(mode==9?s.runtime:s.error),"parent failure identity");
            check(Trace.stage==1 && s.actions==(mode==8?0:1) && divisor(base)==0 && remainder(base)==0,"parent failure leaves default suffix fields");
        } else if(invalid) {
            check(failure==s.invalid && !Trace.condition && Trace.stage==2 && s.actions==1
                    && divisor(base)==0 && remainder(base)==0,"retained prefix scalar and failed suffix guard");
        } else if(mode==11 || mode==12) {
            check(failure==(mode==11?s.runtime:s.error) && divisor(base)==s.divisor && remainder(base)==0
                    && Trace.stage==2 && s.actions==1,"late getter partial fields and failure identity");
        } else {
            check(failure==null && Trace.stage==2 && s.actions==1
                    && divisor(base)==s.divisor && remainder(base)==(kind==2?late/s.divisor:late%s.divisor)
                    && result==(route<3?base:mode==13?changed:marker),"late result and exact constructor return");
            if(kind==0)check(Trace.condition,"prefix scalar survives the superclass call");
            if(mode==2)check(base.result==(kind==1?Integer.MIN_VALUE:0),"integer overflow rule");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++)attempt(route,mode,attempt);
        System.out.println("attempts=2,guarded-integer-division=matched");
    }
}
