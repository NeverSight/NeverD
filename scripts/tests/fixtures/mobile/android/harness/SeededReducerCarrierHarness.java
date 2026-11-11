import fixture.Bucket;
import fixture.Core;
import fixture.Trace;
import java.util.function.BinaryOperator;
import java.util.function.BiConsumer;
import java.util.function.Supplier;
import java.util.stream.Collector;

public final class SeededReducerCarrierHarness {
    private static void check(boolean value, String detail) {
        if (!value) throw new AssertionError("owned seeded reducer carrier mismatch: " + detail);
    }
    private static final class State {
        Object seed, first = new Object(), second = new Object();
        Object left = new Object(), right = new Object(), end = new Object();
        int phase, operators, actions;
        final RuntimeException failure = new IllegalStateException("owned runtime");
        final Error error = new LinkageError("owned error");
    }
    private static Object custom(int route,Object seed,BinaryOperator<Object> operator) {
        switch(route) {
            case 0:return Core.raw(seed,operator);
            case 1:return Core.ignored(seed,operator);
            case 2:return Core.typed(seed,operator);
            case 3:return Core.alias(seed,operator);
            case 4:return Core.reverse(seed,operator);
            default:throw new AssertionError("owned route");
        }
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static void sdk(State s,int mode,BinaryOperator<Object> operator) {
        Throwable failure=null;Object result=null;
        if(mode==7)s.left=s.right=s.end=null;
        else if(mode==14)s.end=null;
        else if(mode==15)s.end=s.left;
        else if(mode==16)s.end=new Object[]{s.first,s.second};
        try {
            Collector collector=Core.sdk(s.seed,operator);
            check(s.operators==0 && collector.characteristics().isEmpty(),"no eager reduction");
            Supplier supplier=collector.supplier();
            Object a=supplier.get(),b=supplier.get();
            check(a!=b,"fresh opaque supplier states");
            BiConsumer accumulator=collector.accumulator();
            s.phase=1;accumulator.accept(a,s.first);
            s.phase=2;accumulator.accept(b,s.second);
            s.phase=3;Object merged=collector.combiner().apply(a,b);
            result=collector.finisher().apply(merged);
            s.phase=4;
        } catch(Throwable caught) { failure=caught; }
        if(mode==2 || mode==3)check(failure instanceof NullPointerException && s.phase==1 && s.operators==0,"null operator after creation");
        else if(mode==5 || mode==6)check(failure==(mode==5?s.failure:s.error) && s.phase==1 && s.operators==1,"accumulator failure identity");
        else if(mode==17)check(failure==s.failure && s.phase==3 && s.operators==3,"combiner failure identity");
        else check(failure==null && s.phase==4 && s.operators==3 && result==s.end,"opaque reduction result");
        check(Trace.calls==0 && Trace.reverses==0 && Trace.stage==0 && s.actions==0,"SDK call uses its own declaration");
    }
    private static void attempt(final int route,final int mode,int attempt) {
        final State s=new State();
        final Object marker=new Object(),poison=new Object();
        final Bucket<Object,Object,Object> selected=new Bucket<Object,Object,Object>(new Object());
        final Bucket<Object,Object,Object> late=new Bucket<Object,Object,Object>(new Object());
        s.seed=mode==1 || mode==3?null:mode==12?selected:mode==13?new Object[]{poison}:new Object();
        final BinaryOperator<Object> operator=mode==2 || mode==3?null:new BinaryOperator<Object>() {
            public Object apply(Object left,Object right) {
                s.operators++;
                check(route==5 && s.phase>=1 && s.phase<=3,"operator only in explicit collector driver");
                check(left==(s.phase<3?s.seed:s.left) && right==(s.phase==1?s.first:s.phase==2?s.second:s.right),"original reduction operands");
                if(mode==5 && s.phase==1 || mode==17 && s.phase==3)throw s.failure;
                if(mode==6 && s.phase==1)throw s.error;
                return s.phase==1?s.left:s.phase==2?s.right:s.end;
            }
        };
        Trace.calls=Trace.reverses=Trace.stage=0;
        Trace.seed=Trace.operator=null;Trace.marker=marker;
        Trace.value=mode==7?null:mode==8?poison:mode==12?s.seed:selected;
        Trace.action=mode==4?null:new Runnable() {
            public void run() {
                s.actions++;
                check(Trace.calls==1 && Trace.stage==1 && Trace.seed==s.seed
                        && Trace.operator==operator && s.operators==0,"original input snapshots");
                if(mode==5)throw s.failure;
                if(mode==6)throw s.error;
                if(mode==9)Trace.value=late;
                if(mode==10){Trace.seed=poison;Trace.operator=poison;}
                if(mode==11)Trace.marker=poison;
            }
        };
        if(route==5){sdk(s,mode,operator);return;}
        Object result=null;Throwable failure=null;
        try {result=custom(route,s.seed,operator);}catch(Throwable caught){failure=caught;}
        if(mode==4)check(failure instanceof NullPointerException,"null callback");
        else if(mode==5 || mode==6)check(failure==(mode==5?s.failure:s.error),"callback failure identity");
        else if(mode==8)check(failure instanceof ClassCastException,"ignored result cast still executes");
        else check(failure==null && result==(route==1?mode==11?poison:marker:mode==7?null:mode==9?late:mode==12?s.seed:selected),"actual carrier reference");
        check(Trace.calls==1 && Trace.reverses==(route==4?1:0)
                && Trace.stage==(mode>=4 && mode<=6?1:2),"one exact provider and continuation");
        check(s.actions==(mode==4?0:1) && s.operators==0,"no inferred operator call");
        check(Trace.seed==(mode==10?poison:s.seed)
                && Trace.operator==(mode==10?poison:operator),"late writes keep original inputs");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++)attempt(route,mode,attempt);
        System.out.println("attempts=2,seeded-reducer-carrier=matched");
    }
}
