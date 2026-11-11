package fixture;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class PairedAdapterInputsHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final String SEED=new String("owned-seed"), RESULT=new String("owned-result");
    static final Object OWNER=new Object(), PAIRED=new Object(), MAPPED=new Object(), LATE=new Object();
    static final long STAMP=0x12345678fedcba98L;
    static final int DIRECTION=-17;
    static int route,mode;
    static Object owner,seed,pairArg,singleArg,contextArg,result;
    static Context context;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned paired adapter inputs mismatch");
    }
    static final class Pair implements Provider.Pair<Object,Object> {
        public Object apply(Object a,Object b) {
            check(a==owner && b==seed);
            events.add(6);
            if(mode==8)throw FAILURE;
            return PAIRED;
        }
    }
    static final class Single implements Provider.Single<Object> {
        public Object apply(Object value) {
            check(value==PAIRED);
            events.add(7);
            if(mode==9)throw FAILURE;
            return MAPPED;
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object a,Object p,Object s,Object b,Object c,Object value,int d,long clock,boolean forward,boolean wrap) {
            check(a==owner && p==pairArg && s==singleArg && b==seed);
            check(c==((route&1)==0?contextArg:null));
            check(d==DIRECTION && clock==((route&1)==0?0L:STAMP));
            check(forward==((route&1)!=0) && !wrap);
            Object wanted=phase==2?PAIRED:phase==3?MAPPED:phase==4&&route<2?result:null;
            check(value==wanted);
            events.add(phase);
            if((mode==10&&phase==1)||(mode==11&&phase==2)||(mode==12&&phase==3)||(mode==15&&phase==4)||(mode==18&&phase==0))throw FAILURE;
            if(phase==3) {
                context.reply=LATE;
                Trace.reply=LATE;
            }
        }
    }
    static void attempt() {
        events.clear();
        owner=mode==1?null:OWNER;
        seed=mode==4?null:SEED;
        pairArg=mode==2?null:mode==6?new Object():new Pair();
        singleArg=mode==3?null:mode==7?new Object():new Single();
        context=new Context();
        contextArg=mode==5?null:mode==17?new Object():context;
        result=mode==13?new Object():mode==14?null:mode==19?seed:RESULT;
        context.reply=result;
        Trace.reply=result;
        Trace.hook=new Observer();
        Object actual=null;
        Throwable failure=null;
        try {
            if(route==0)actual=Core.run(owner,pairArg,singleArg,seed,contextArg,DIRECTION);
            else if(route==1)actual=Core.relative(owner,pairArg,singleArg,seed,STAMP,DIRECTION,true,false);
            else if(route==2)Core.ignore(owner,pairArg,singleArg,seed,contextArg,DIRECTION);
            else Core.ignoreRelative(owner,pairArg,singleArg,seed,STAMP,DIRECTION,true,false);
        } catch(Throwable caught) { failure=caught; }
        int[] expected={0,1,6,2,7,3,4};
        Class<?> kind=null;
        boolean exact=false;
        if(mode==6||mode==7||(mode==17&&(route&1)==0)) { expected=new int[]{};kind=ClassCastException.class; }
        else if(mode==18) {expected=new int[]{0};exact=true;}
        else if(mode==10) {expected=new int[]{0,1};exact=true;}
        else if(mode==2) {expected=new int[]{0,1};kind=NullPointerException.class;}
        else if(mode==8) {expected=new int[]{0,1,6};exact=true;}
        else if(mode==11) {expected=new int[]{0,1,6,2};exact=true;}
        else if(mode==3) {expected=new int[]{0,1,6,2};kind=NullPointerException.class;}
        else if(mode==9) {expected=new int[]{0,1,6,2,7};exact=true;}
        else if(mode==5&&(route&1)==0) {expected=new int[]{0,1,6,2,7};kind=NullPointerException.class;}
        else if(mode==12) {expected=new int[]{0,1,6,2,7,3};exact=true;}
        else if(mode==15) {exact=true;}
        else if(mode==13&&route<2) {kind=ClassCastException.class;}
        List<Integer> wanted=new ArrayList<Integer>();
        for(int event:expected)wanted.add(event);
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else {check(failure==null);check(actual==(route<2?result:null));}
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);
        attempt();attempt();
        System.out.println("attempts=2,paired-adapter-inputs=matched");
    }
}
