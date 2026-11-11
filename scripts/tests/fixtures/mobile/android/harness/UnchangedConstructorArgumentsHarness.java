package fixture;
import java.util.ArrayList;
import java.util.List;

public final class UnchangedConstructorArgumentsHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static final Base OTHER=new Base(null,-1);
    static int branch,mode,selected,late;
    static Child source,created;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned unchanged constructor arguments mismatch");
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object value,Base input,int code) {
            check(code==selected);
            if(phase==1)check(value==null&&input==null);
            else {
                if(phase==2)created=(Child)value;
                check(value==created&&input==source);
                check(created.value==(phase==5?late:0));
                if(phase==2)check(created.source==null&&created.code==0);
                if(phase==3)check(created.source==source&&created.code==selected);
                if(phase>=4)check(created.source==OTHER&&created.code==selected);
            }
            events.add(phase);
            if(mode>=2&&mode<=6&&phase==mode-1)throw FAILURE;
            if(phase==3) {
                if(source!=null)source.value=late;
                created.source=OTHER;
            }
        }
    }
    static void attempt() {
        events.clear();created=null;source=mode==1?null:new Child(mode==7?Integer.MIN_VALUE:21);
        late=mode==7?Integer.MAX_VALUE:37;selected=branch==1?17:29;
        Trace.hook=new Observer();Object answer=null;Throwable failure=null;
        try {answer=new Child(source,branch==1);}catch(Throwable caught){failure=caught;}
        int[] expected={1,2,3,4,5};Class<?> kind=null;boolean exact=false;
        if(mode>=2&&mode<=6) {
            int count=mode-1;expected=new int[count];for(int i=0;i<count;i++)expected[i]=i+1;exact=true;
        } else if(mode==1) {expected=new int[]{1,2,3,4};kind=NullPointerException.class;}
        List<Integer> wanted=new ArrayList<Integer>();for(int event:expected)wanted.add(event);
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else {check(failure==null&&answer==created&&created.value==late&&source.value==late);}
        Trace.hook=null;
    }
    public static void main(String[] args) {
        branch=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);attempt();attempt();
        System.out.println("attempts=2,unchanged-constructor-arguments=matched");
    }
}
