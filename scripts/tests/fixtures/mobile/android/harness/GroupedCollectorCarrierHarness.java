import fixture.Bucket;
import fixture.Core;
import fixture.Trace;
import java.util.HashSet;
import java.util.Set;
import java.util.Map;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.function.BinaryOperator;
import java.util.function.BiConsumer;
import java.util.stream.Collector;

public final class GroupedCollectorCarrierHarness {
    private static void check(boolean value,String detail) {
        if(!value)throw new AssertionError("owned grouped collector carrier mismatch: "+detail);
    }
    private static final class Cell {}
    private static final class State {
        int mode,phase,getters,suppliers,mappers,accumulators,combiners,finishers,actions;
        final Object first=new Object(),second=new Object(),answer=new Object();
        Object mappedFirst=new Object(),mappedSecond;
        final Cell a=new Cell(),b=new Cell(),merged=new Cell();
        final RuntimeException failure=new IllegalStateException("owned runtime");
        final Error error=new LinkageError("owned error");
        final Set<Collector.Characteristics> flags=new HashSet<Collector.Characteristics>();
        Supplier<Cell> supplier;
        BiConsumer<Cell,Object> accumulator;
        BinaryOperator<Cell> combiner;
        Function<Cell,Object> finisher;
        void getter(int next) {
            check(phase==0 && next==getters+1,"downstream accessor order");getters++;
            if(mode==4&&next==1 || mode==5&&next==2 || mode==7&&next==4)throw failure;
            if(mode==6&&next==3 || mode==8&&next==5)throw error;
        }
        State(int mode) {
            this.mode=mode;mappedSecond=mappedFirst;
            if(mode==16)mappedFirst=mappedSecond=null;
            if(mode==20){mappedFirst=first;mappedSecond=second;}
            if(mode==21)flags.add(Collector.Characteristics.IDENTITY_FINISH);
            supplier=() -> {
                suppliers++;check(phase==suppliers && mappers==phase,"supplier after classifier");
                return mode==15?null:suppliers==1?a:b;
            };
            accumulator=(cell,value) -> {
                accumulators++;
                check((phase==1 || phase==2) && mappers==phase
                        && cell==(mode==15?null:phase==1?a:b)
                        && value==(phase==1?first:second),"original downstream input");
                if(mode==11)throw failure;
                if(mode==12)throw error;
            };
            combiner=(left,right) -> {
                combiners++;check(phase==3 && left==a && right==b,"opaque combiner states");
                if(mode==13)throw failure;
                return merged;
            };
            finisher=cell -> {
                finishers++;check(phase==4 && (mode==20?cell==a || cell==b:cell==merged),"opaque finisher state");
                if(mode==14)throw error;
                return answer;
            };
        }
    }
    private static final class Downstream implements Collector<Object,Cell,Object> {
        final State s;
        Downstream(State s){this.s=s;}
        public Supplier<Cell> supplier(){s.getter(1);return s.supplier;}
        public BiConsumer<Cell,Object> accumulator(){s.getter(2);return s.accumulator;}
        public BinaryOperator<Cell> combiner(){s.getter(3);return s.combiner;}
        public Function<Cell,Object> finisher(){s.getter(5);return s.mode==23?null:s.finisher;}
        public Set<Characteristics> characteristics(){s.getter(4);return s.mode==22?null:s.flags;}
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static void sdk(State s,Function<Object,Object> mapper) {
        Collector down=s.mode==2 || s.mode==3?null:new Downstream(s);
        Throwable failure=null;Object result=null,combined=null;
        try {
            Collector collector=Core.sdk(mapper,down);
            check(s.getters==(s.mode==21?4:5) && s.mappers==0,"creation captures complete downstream");
            if(s.mode==18) {
                s.supplier=() -> {throw new AssertionError("late supplier");};
                s.accumulator=(a,b) -> {throw new AssertionError("late accumulator");};
                s.combiner=(a,b) -> {throw new AssertionError("late combiner");};
                s.finisher=a -> {throw new AssertionError("late finisher");};
            }
            if(s.mode==17)s.flags.add(Collector.Characteristics.CONCURRENT);
            if(s.mode==21)s.flags.clear();
            Set<Collector.Characteristics> flags=collector.characteristics();
            check(flags==collector.characteristics() && flags.size()==(s.mode==21?1:0)
                    && flags.contains(Collector.Characteristics.IDENTITY_FINISH)==(s.mode==21),"captured identity finish policy");
            Object a=collector.supplier().get(),b=collector.supplier().get();
            check(a instanceof Map && b instanceof Map && a!=b && s.suppliers==0,"opaque empty map suppliers");
            s.phase=1;collector.accumulator().accept(a,s.first);
            s.phase=2;collector.accumulator().accept(b,s.second);
            s.phase=3;combined=collector.combiner().apply(a,b);
            check(combined==a,"original left map identity");
            s.phase=4;result=collector.finisher().apply(combined);
            s.phase=5;
        }catch(Throwable caught){failure=caught;}
        if(s.mode==2 || s.mode==3)check(failure instanceof NullPointerException && s.phase==0 && s.getters==0,"null downstream before classifier");
        else if(s.mode>=4 && s.mode<=8)check(failure==(s.mode==6 || s.mode==8?s.error:s.failure)
                && s.phase==0 && s.getters==s.mode-3,"accessor failure identity and prefix");
        else if(s.mode==22)check(failure instanceof NullPointerException && s.phase==0 && s.getters==4,"null characteristics");
        else if(s.mode==1)check(failure instanceof NullPointerException && s.phase==1 && s.mappers==0 && s.accumulators==0,"null classifier after creation");
        else if(s.mode==9 || s.mode==10)check(failure==(s.mode==9?s.failure:s.error)
                && s.phase==1 && s.mappers==1 && s.accumulators==0,"classifier before downstream failure");
        else if(s.mode==16)check(failure instanceof NullPointerException && s.phase==1 && s.mappers==1 && s.suppliers==0 && s.accumulators==0,"null key before downstream supplier");
        else if(s.mode==11 || s.mode==12)check(failure==(s.mode==11?s.failure:s.error)
                && s.phase==1 && s.mappers==1 && s.accumulators==1,"downstream failure identity");
        else if(s.mode==13)check(failure==s.failure && s.phase==3 && s.combiners==1,"combiner failure");
        else if(s.mode==14)check(failure==s.error && s.phase==4 && s.finishers==1,"finisher failure");
        else if(s.mode==23)check(failure instanceof NullPointerException && s.phase==4 && s.finishers==0,"captured null finisher");
        else {
            check(failure==null && s.phase==5 && s.mappers==2 && s.accumulators==2
                    && s.combiners==(s.mode==15 || s.mode==20?0:1)
                    && s.finishers==(s.mode==15 || s.mode==21?0:s.mode==20?2:1)
                    && result==combined,"opaque grouped result identity");
            Map values=(Map)result;
            check(values.size()==(s.mode==15?0:s.mode==20?2:1),"actual key count");
            if(s.mode!=15)check(values.get(s.mappedFirst)==(s.mode==21?s.merged:s.answer),"actual nested value");
            if(s.mode==20)check(values.get(s.mappedSecond)==s.answer,"second original key");
        }
        if(s.phase>0)check(s.getters==(s.mode==21?4:5)
                && s.suppliers==(s.phase==1?s.accumulators==0?0:1:2),"captured functions stay fixed");
        check(Trace.calls==0 && Trace.reverses==0 && Trace.stage==0 && s.actions==0,"SDK uses its own declaration");
    }
    private static Object custom(int route,Function<Object,Object> mapper,Bucket<Object,Object,Object> downstream) {
        switch(route) {
            case 0:return Core.raw(mapper,downstream);
            case 1:return Core.ignored(mapper,downstream);
            case 2:return Core.typed(mapper,downstream);
            case 3:return Core.alias(mapper,downstream);
            case 4:return Core.reverse(mapper,downstream);
            default:throw new AssertionError("owned route");
        }
    }
    private static void attempt(final int route,final int mode,int attempt) {
        final State s=new State(mode);
        final Object poison=new Object(),marker=new Object();
        final Bucket<Object,Object,Object> downstream=mode==2 || mode==3?null:new Bucket<Object,Object,Object>(new Object());
        final Bucket<Object,Object,Object> selected=new Bucket<Object,Object,Object>(new Object()),late=new Bucket<Object,Object,Object>(new Object());
        final Function<Object,Object> mapper=mode==1 || mode==3?null:new Function<Object,Object>() {
            public Object apply(Object value) {
                s.mappers++;
                check(route==5 && (s.phase==1 || s.phase==2)
                        && value==(s.phase==1?s.first:s.second),"original mapper input");
                if(mode==9)throw s.failure;
                if(mode==10)throw s.error;
                if(mode==19)s.accumulator=(a,b) -> {throw new AssertionError("late accumulator");};
                return s.phase==1?s.mappedFirst:s.mappedSecond;
            }
        };
        Trace.calls=Trace.reverses=Trace.stage=0;
        Trace.mapper=Trace.downstream=null;Trace.marker=marker;
        Trace.value=mode==7?null:mode==8?poison:mode==12?downstream:selected;
        Trace.action=mode==4?null:() -> {
            s.actions++;
            check(Trace.calls==1 && Trace.stage==1 && Trace.mapper==mapper
                    && Trace.downstream==downstream && s.mappers==0,"factory input snapshots");
            if(mode==5)throw s.failure;
            if(mode==6)throw s.error;
            if(mode==9)Trace.value=late;
            if(mode==10){Trace.mapper=poison;Trace.downstream=poison;}
            if(mode==11)Trace.marker=poison;
        };
        if(route==5){sdk(s,mapper);return;}
        Object value=null;Throwable failure=null;
        try{value=custom(route,mapper,downstream);}catch(Throwable caught){failure=caught;}
        if(mode==4)check(failure instanceof NullPointerException,"null action");
        else if(mode==5 || mode==6)check(failure==(mode==5?s.failure:s.error),"action failure identity");
        else if(mode==8)check(failure instanceof ClassCastException,"ignored result cast");
        else check(failure==null && value==(route==1?mode==11?poison:marker
                :mode==7?null:mode==9?late:mode==12?downstream:selected),"actual carrier reference");
        check(Trace.calls==1 && Trace.reverses==(route==4?1:0)
                && Trace.stage==(mode>=4 && mode<=6?1:2),"one factory and continuation");
        check(s.actions==(mode==4?0:1) && s.mappers==0,"no inferred mapper");
        check(Trace.mapper==(mode==10?poison:mapper)
                && Trace.downstream==(mode==10?poison:downstream),"late factory writes");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++)attempt(route,mode,attempt);
        System.out.println("attempts=2,grouped-collector-carrier=matched");
    }
}
