package fixture;
import java.util.ArrayList;
import java.util.List;

public final class BoundedProducerTokenHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final String VALUE=new String("owned-result");
    static int route,eager,mode;
    static Context context;
    static Token<Bound> token;
    static ProducerProbe first,second,third;
    static Result<Bound> result,late;
    static final Store STORE=new Store();static final Extras EXTRAS=new Extras();
    static final FactoryProbe FACTORY=new FactoryProbe();
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {if(!value)throw new AssertionError("owned bounded producer token mismatch");}
    static final class FactoryProbe implements Context.Factory {public Object marker(){return this;}}
    static final class ProducerProbe implements Producer<Object> {
        final int kind;ProducerProbe(int kind){this.kind=kind;}
        public Object get() {
            events.add(6+kind);if(mode==5+kind)throw FAILURE;
            if(kind==2)context.reply=result;
            return kind==0?STORE:kind==1?(mode==8?new Object():EXTRAS):FACTORY;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object c,Object t,Object a,Object b,Object d,Object seen) {
            check(c==context);
            if(phase==2)check(t==STORE&&a==EXTRAS&&b==FACTORY&&d==token&&seen==null);
            else check(t==token&&a==first&&b==second&&d==third&&seen==(phase==3||phase==4&&route==0?result:null));
            events.add(phase);
            if(mode==12&&phase==1||mode==13&&phase==3||mode==14&&phase==4)throw FAILURE;
            if(phase==3)context.reply=late;
        }
    }
    static void attempt() {
        events.clear();context=mode==1?null:new Context();token=mode==11?null:new Token<Bound>();
        result=mode==10?null:new Result<Bound>(mode==9?new Object():VALUE);late=new Result<Bound>(new Object());
        if(context!=null){context.eager=eager==1;context.reply=eager==1?new Result<Bound>(new Object()):result;}
        first=mode==2?null:new ProducerProbe(0);second=mode==3?null:new ProducerProbe(1);third=mode==4?null:new ProducerProbe(2);
        Trace.hook=new Observer();Object answer=null;Throwable failure=null;
        try {if(route==0)answer=Api.run(context,token,first,second,third);else Api.ignore(context,token,first,second,third);}catch(Throwable caught){failure=caught;}
        List<Integer> wanted=new ArrayList<Integer>();wanted.add(0);wanted.add(1);
        if(eager==1){wanted.add(6);wanted.add(7);wanted.add(8);wanted.add(2);}wanted.add(3);wanted.add(4);
        Class<?> kind=null;boolean exact=false;int last=-1;
        if(mode==1){kind=NullPointerException.class;last=1;}
        else if(mode>=2&&mode<=4&&eager==1){kind=NullPointerException.class;last=mode==2?1:mode==3?6:7;}
        else if(mode>=5&&mode<=7&&eager==1){exact=true;last=mode+1;}
        else if(mode==8&&eager==1){kind=ClassCastException.class;last=7;}
        else if(mode==12){exact=true;last=1;}
        else if(mode==13){exact=true;last=3;}
        else if(mode==14){exact=true;last=4;}
        else if(mode==9&&route==0)kind=ClassCastException.class;
        else if(mode==10&&route==0)kind=NullPointerException.class;
        if(last>=0)wanted=new ArrayList<Integer>(wanted.subList(0,wanted.indexOf(last)+1));
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);else if(kind!=null)check(failure!=null&&failure.getClass()==kind);else check(failure==null&&answer==(route==0?VALUE:null));
        Trace.hook=null;
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);eager=Integer.parseInt(args[1]);mode=Integer.parseInt(args[2]);attempt();attempt();
        System.out.println("attempts=2,bounded-producer-token=matched");
    }
}
