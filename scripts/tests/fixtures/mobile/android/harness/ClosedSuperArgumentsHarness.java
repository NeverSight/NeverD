import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedSuperArgumentsHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned closed super arguments"); }
    static final class Callback implements Fn {
        public Object apply(Object input) {
            Trace.seenCallback=this;Trace.seenInput=input;Trace.hit(9);return Trace.reply;
        }
    }
    static Child build(int route,Provider p,Fn<Object,Unit> a,Fn<Object,Unit> b,Object token) throws Throwable {
        try {
            if(route==0)return Client.build(p,a,b,token);
            if(route==1)return ClosedSuperArgumentsCaller.build(p,a,b,token);
            if(route==2)return Child.class.getConstructor(Provider.class,Fn.class,Fn.class,Object.class).newInstance(p,a,b,token);
            return (Child)MethodHandles.publicLookup().findConstructor(Child.class,
                    MethodType.methodType(void.class,Provider.class,Fn.class,Fn.class,Object.class)).invokeExact(p,a,b,token);
        } catch(InvocationTargetException e) { throw e.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),variant=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        boolean useWrite=(variant&8)!=0,missing=(variant&4)!=0;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.fail=mode>=1 && mode<=9 ? mode : 0;
            Trace.failure=new IllegalArgumentException("owned sentinel");
            Trace.partial=null;Trace.seenCallback=null;Trace.seenInput=null;
            Trace.bits=attempt==0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Trace.reply=mode==10 ? new Object() : mode==11 ? null : new Unit();
            Fn<Object,Unit> a=(variant&1)!=0 ? new Callback() : null;
            Fn<Object,Unit> b=(variant&2)!=0 ? new Callback() : null;
            Provider provider=new Provider();provider.first=new Callback();provider.second=new Callback();
            Trace.first=new Callback();Trace.second=new Callback();
            Object token=mode==12 ? null : new Object(),input=mode==13 ? null : new Object();
            Object first=a!=null ? a : missing ? Trace.first : provider.first;
            Object second=b!=null ? b : missing ? Trace.second : provider.second;
            int[] points={1,a==null ? missing ? 3 : 2 : 0,b==null ? missing ? 5 : 4 : 0,6,7,8,9};
            long expected=0;int last=0;boolean failed=false;
            for(int point:points) {
                if(point==0)continue;
                expected=expected*31+point;last=point;
                if(point==Trace.fail) { failed=true;break; }
            }
            Child child=null;Object result=null;Throwable caught=null;
            try {
                child=build(route,missing ? null : provider,a,b,token);
                result=route==0 ? Client.call(child,input,useWrite) : ClosedSuperArgumentsCaller.call(child,input,useWrite);
            } catch(Throwable e) { caught=e; }
            require(Trace.events==expected);
            if(failed)require(caught==Trace.failure);
            else if(mode==10)require(caught instanceof ClassCastException);
            else require(caught==null && result==Trace.reply);
            boolean stored=!failed || last>=7;
            require((Trace.partial!=null)==stored);
            if(stored) {
                Parent partial=Trace.partial;
                require(partial.bits==Trace.bits && partial.token==token && partial.first==first && partial.second==second);
                require(((Child)partial).marker==(failed && last==7 ? null : token));
            }
            require((child!=null)==(!failed || last>=8));
            require(Trace.seenCallback==(last==9 ? useWrite ? second : first : null));
            require(Trace.seenInput==(last==9 ? input : null));
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",events="+expected);
        }
    }
}
