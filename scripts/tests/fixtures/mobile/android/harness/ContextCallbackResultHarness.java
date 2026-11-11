package fixture;
import java.util.ArrayList;
import java.util.List;

public final class ContextCallbackResultHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final String RESULT=new String("owned-result");
    static final Object LATE=new Object();
    static final int LEFT=-19,RIGHT=Integer.MIN_VALUE,LIMIT=Integer.MAX_VALUE;
    static int route,mode;
    static Provider owner;
    static CharSequence context;
    static Provider.Adapter callback;
    static Object result;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned context callback result mismatch");
    }
    static final class Probe extends Provider.Adapter {
        public void accept(CharSequence value,int left,int right,int limit,boolean flag) {
            check(value==context&&left==LEFT&&right==RIGHT&&limit==LIMIT&&flag);
            events.add(6);if(mode==8)throw FAILURE;
        }
        public Object getResult() {
            events.add(5);if(mode==9)throw FAILURE;
            return super.getResult();
        }
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object value,CharSequence text,Object input,int left,int right,int limit,boolean flag,Object seen) {
            check(value==owner&&text==context&&input==callback);
            check(left==LEFT&&right==RIGHT&&limit==LIMIT&&flag);
            check(seen==(phase==3||phase==4&&route==0?result:null));
            events.add(phase);
            if(mode==5&&phase==1||mode==6&&phase==3||mode==7&&phase==4)throw FAILURE;
            if(phase==3&&callback!=null)callback.reply=LATE;
        }
    }
    static void attempt() {
        events.clear();owner=mode==13?null:new Provider();
        context=mode==1?null:new StringBuilder("owned-context");
        callback=mode==2?null:new Probe();result=mode==10?new Object():mode==11?null:RESULT;
        if(callback!=null)callback.reply=result;
        Object text=mode==3?new Object():context,input=mode==4?new Object():callback;
        Trace.hook=new Observer();Object answer=null;Throwable failure=null;
        try {
            if(route==0)answer=owner.run(text,input,LEFT,RIGHT,LIMIT,true);
            else owner.ignore(text,input,LEFT,RIGHT,LIMIT,true);
        } catch(Throwable caught) {failure=caught;}
        int[] expected={0,1,6,5,3,4};Class<?> kind=null;boolean exact=false;
        if(mode==13) {expected=new int[]{};kind=NullPointerException.class;}
        else if(mode==3||mode==4) {expected=new int[]{};kind=ClassCastException.class;}
        else if(mode==5) {expected=new int[]{0,1};exact=true;}
        else if(mode==2) {expected=new int[]{0,1};kind=NullPointerException.class;}
        else if(mode==8) {expected=new int[]{0,1,6};exact=true;}
        else if(mode==9) {expected=new int[]{0,1,6,5};exact=true;}
        else if(mode==6) {expected=new int[]{0,1,6,5,3};exact=true;}
        else if(mode==7) {exact=true;}
        else if(mode==10&&route==0) {kind=ClassCastException.class;}
        List<Integer> wanted=new ArrayList<Integer>();for(int event:expected)wanted.add(event);
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else {check(failure==null);check(answer==(route==0?result:null));}
        Trace.hook=null;
    }
    public static void main(String[] args) {
        route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);attempt();attempt();
        System.out.println("attempts=2,context-callback-result=matched");
    }
}
