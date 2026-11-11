package fixture;
import java.util.ArrayList;
import java.util.List;

public final class ErasedSuperTokenResultHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final Object[] LATE=new Object[]{new Object()};
    static final int LEFT=-19,RIGHT=Integer.MIN_VALUE;
    static int route,mode;
    static Child owner;
    static Object token;
    static Object[] result;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned erased super token result mismatch");
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object value,Object selected,int left,int right,Object seen) {
            check(value==owner&&left==LEFT&&right==RIGHT);
            check(selected==(phase==2||phase==3?(token==null?String.class:token):token));
            check(seen==(phase>=2&&phase<=4||phase==5&&route==0?result:null));
            events.add(phase);
            if(mode>=5&&mode<=9&&phase==mode-4)throw FAILURE;
            if(phase==3)owner.reply=LATE;
        }
    }
    static void attempt() {
        events.clear();owner=mode==4?null:new Child();
        token=mode==1?Integer.class:mode==2?null:String.class;
        result=mode==10?new Object[]{new Object()}:mode==11?null:new String[]{new String("owned-result")};
        if(owner!=null)owner.reply=result;
        Object input=mode==3?new Object():token;
        Trace.hook=new Observer();Object answer=null;Throwable failure=null;
        try {
            if(route==0)answer=owner.run(input,LEFT,RIGHT);
            else owner.ignore(input,LEFT,RIGHT);
        } catch(Throwable caught) {failure=caught;}
        int[] expected={0,1,2,3,4,5};Class<?> kind=null;boolean exact=false;
        if(mode==4) {expected=new int[]{};kind=NullPointerException.class;}
        else if(mode==3) {expected=new int[]{};kind=ClassCastException.class;}
        else if(mode>=5&&mode<=9) {
            int last=mode-4;expected=new int[last+1];for(int i=0;i<=last;i++)expected[i]=i;exact=true;
        }
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
        System.out.println("attempts=2,erased-super-token-result=matched");
    }
}
