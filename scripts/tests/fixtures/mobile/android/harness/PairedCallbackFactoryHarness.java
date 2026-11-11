package fixture;
import java.util.ArrayList;
import java.util.List;

public final class PairedCallbackFactoryHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final Object EARLY=new Object(),OUTPUT=new Object();
    static final String VALUE=new String("owned-result");
    static int route,mode;
    static Host owner;
    static Pair<Object,Object> input;
    static FunctionProbe context;
    static CallbackProbe callback;
    static Box<Object> result,late;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned paired callback factory mismatch");
    }
    static final class Derived extends Host {
        public <I,O> Box<I> make(Pair<I,O> pair,Function<Void,Registry> function,Callback<O> consumer) {
            throw new AssertionError("owned paired callback factory mismatch");
        }
    }
    static final class FunctionProbe implements Function<Object,Object> {
        public Object apply(Object value) {
            check(value==null);events.add(6);if(mode==7)throw FAILURE;
            if(input!=null)input.output=OUTPUT;
            return mode==9?new Object():new Registry();
        }
    }
    static final class CallbackProbe implements Callback<Object> {
        public void accept(Object value) {
            check(value==OUTPUT);events.add(7);if(mode==8)throw FAILURE;
            input.box=result;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object value,Object pair,Object function,Object consumer,Object seen) {
            check(value==owner&&pair==input&&function==context&&consumer==callback);
            check(seen==(phase==3||phase==4&&route==0?result:null));events.add(phase);
            if(mode==10&&phase==1||mode==11&&phase==3||mode==12&&phase==4)throw FAILURE;
            if(phase==3&&input!=null)input.box=late;
        }
    }
    static void attempt() {
        events.clear();owner=mode==15?new Derived():new Host();input=mode==1?null:new Pair<Object,Object>();
        result=mode==14?null:new Box<Object>(mode==13?new Object():VALUE);late=new Box<Object>(new Object());
        if(input!=null){input.output=EARLY;input.box=new Box<Object>(new Object());}
        context=mode==2?null:new FunctionProbe();callback=mode==3?null:new CallbackProbe();
        Object pair=mode==4?new Object():input,function=mode==5?new Object():context,consumer=mode==6?new Object():callback;
        Trace.hook=new Observer();Object answer=null;Throwable failure=null;
        try {if(route==0)answer=owner.run(pair,function,consumer);else owner.ignore(pair,function,consumer);}catch(Throwable caught){failure=caught;}
        int[] expected={0,1,6,7,3,4};Class<?> kind=null;boolean exact=false;
        if(mode>=4&&mode<=6){expected=new int[]{};kind=ClassCastException.class;}
        else if(mode==1){expected=new int[]{0,1,6};kind=NullPointerException.class;}
        else if(mode==2){expected=new int[]{0,1};kind=NullPointerException.class;}
        else if(mode==3){expected=new int[]{0,1,6};kind=NullPointerException.class;}
        else if(mode==7){expected=new int[]{0,1,6};exact=true;}
        else if(mode==8){expected=new int[]{0,1,6,7};exact=true;}
        else if(mode==9){expected=new int[]{0,1,6};kind=ClassCastException.class;}
        else if(mode==10){expected=new int[]{0,1};exact=true;}
        else if(mode==11){expected=new int[]{0,1,6,7,3};exact=true;}
        else if(mode==12){exact=true;}
        else if(mode==13&&route==0){kind=ClassCastException.class;}
        else if(mode==14&&route==0){kind=NullPointerException.class;}
        List<Integer> wanted=new ArrayList<Integer>();for(int event:expected)wanted.add(event);check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else {check(failure==null&&answer==(route==0?VALUE:null));}
        Trace.hook=null;
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);attempt();attempt();
        System.out.println("attempts=2,paired-callback-factory=matched");
    }
}
