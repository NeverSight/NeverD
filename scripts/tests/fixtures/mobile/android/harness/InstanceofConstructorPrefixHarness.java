package fixture;
import java.util.ArrayList;
import java.util.List;
import java.lang.reflect.Modifier;

public final class InstanceofConstructorPrefixHarness {
    static final RuntimeException FAILURE=new IllegalArgumentException("owned-failure");
    static int route,variant,mode,code;
    static Context context;
    static Activity activity;
    static Handler handler;
    static Concrete created;
    static final List<Integer> events=new ArrayList<Integer>();
    static void check(boolean value) {
        if(!value)throw new AssertionError("owned instanceof constructor prefix mismatch");
    }
    static final class Observer extends Trace {
        public void observe(int phase,Object owner,Object a,Object c,Object h,int selected) {
            if(phase==8) {
                check(route==2&&owner instanceof Handler&&a==null&&c==null&&h==null&&selected==0);
                handler=(Handler)owner;
            } else if(phase==0) {
                created=(Concrete)owner;check(a==null&&c==null&&h==null&&selected==0);
                check(created.activity==null&&created.context==null&&created.handler==null&&created.code==0&&created.payload==null);
            } else if(phase==3) {
                check(owner==created&&a==activity&&c==context&&h==handler&&selected==code);
                check(created.activity==activity&&created.context==context&&created.handler==handler&&created.code==code&&created.payload==null);
            } else {
                check(owner==null&&a==(phase==1?context:phase==2?handler:context)&&c==null&&h==null&&selected==0);
            }
            events.add(phase);
            if(mode==3&&phase==0||mode==4&&phase==1||mode==5&&phase==2||mode==6&&phase==3||mode==7&&phase==8||mode==8&&phase==6)throw FAILURE;
        }
    }
    static void attempt() throws Exception {
        check(Modifier.isAbstract(Host.class.getModifiers()));
        check(Host.class.getTypeParameters().length==1&&Host.class.getTypeParameters()[0].getName().equals("H"));
        check(Host.class.getField("payload").getGenericType().getTypeName().equals("H"));
        events.clear();created=null;
        context=mode==1?null:route==2||variant==1?new Activity():new Context();
        activity=route==0?new Activity():context instanceof Activity?(Activity)context:null;
        handler=mode==2?null:new Handler();code=route==2?0:mode==9?Integer.MIN_VALUE:37;
        Trace.hook=new Observer();Object answer=null;Throwable failure=null;
        try {
            if(route==0)answer=new Concrete(activity,context,handler,code);
            else if(route==1)answer=new Concrete(context,handler,code);
            else answer=new Concrete((Activity)context);
        }catch(Throwable caught){failure=caught;}
        List<Integer> wanted=new ArrayList<Integer>();
        if(route==2){wanted.add(6);wanted.add(8);}wanted.add(0);wanted.add(1);wanted.add(2);wanted.add(3);
        Class<?> kind=null;boolean exact=false;int last=-1;
        if(mode==1){kind=NullPointerException.class;last=route==2?6:1;}
        else if(mode==2&&route!=2){kind=NullPointerException.class;last=2;}
        else {
            int phase=mode==3?0:mode==4?1:mode==5?2:mode==6?3:mode==7?8:mode==8?6:-1;
            if(wanted.contains(phase)){exact=true;last=phase;}
        }
        if(last>=0)wanted=new ArrayList<Integer>(wanted.subList(0,wanted.indexOf(last)+1));
        check(events.equals(wanted));
        if(exact)check(failure==FAILURE);
        else if(kind!=null)check(failure!=null&&failure.getClass()==kind);
        else check(failure==null&&answer==created&&created.activity==activity&&created.context==context&&created.handler==handler&&created.code==code&&created.payload==null);
        Trace.hook=null;
    }
    public static void main(String[] args) throws Exception {
        route=Integer.parseInt(args[0]);variant=Integer.parseInt(args[1]);mode=Integer.parseInt(args[2]);attempt();attempt();
        System.out.println("attempts=2,instanceof-constructor-prefix=matched");
    }
}
