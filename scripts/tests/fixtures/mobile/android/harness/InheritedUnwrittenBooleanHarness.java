import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InheritedUnwrittenBooleanHarness {
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned inherited unwritten Boolean behavior"); }
    static Child create(int route,long value,int marker,boolean delegate) throws Throwable {
        if(route==0)return Client.create(value,marker,delegate);
        if(route==1)return InheritedUnwrittenBooleanCaller.create(value,marker,delegate);
        if(route==2) {
            try {
                if(delegate)return Child.class.getConstructor(long.class,int.class).newInstance(value,marker);
                return Child.class.getConstructor(long.class).newInstance(value);
            } catch(InvocationTargetException e) { throw e.getCause(); }
        }
        if(delegate) {
            MethodHandle h=MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,long.class,int.class));
            return (Child)h.invokeExact(value,marker);
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,long.class));
        return (Child)h.invokeExact(value);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean encoded=Boolean.parseBoolean(args[2]);
        Field flag=Owner.class.getField("zeroFlag");check(Modifier.isFinal(flag.getModifiers())==encoded);flag.setAccessible(true);
        check(Modifier.isAbstract(Owner.class.getModifiers())&&Owner.class.getSuperclass()==Base.class);
        for(int attempt=0;attempt<2;attempt++) {
            long value=0x8123456789abcdefL+attempt;int marker=13+attempt;
            boolean delegate=mode%2!=0,mutate=mode>=2;
            int failure=mode>=4?(mode-4)/2+1:0;
            if(failure==4&&!delegate)failure=5;
            Trace.input=value;Trace.events=0;Trace.partial=null;Trace.flags=new boolean[6];Trace.values=new long[6];Trace.bases=new long[6];Trace.children=new int[6];
            Trace.failAt=failure;Trace.failure=new RuntimeException("owned failure");
            Trace.hook=mutate?new Hook() {
                public void observe(Owner owner,int stage) {
                    if(stage==1)try { flag.setBoolean(owner,true); }catch(IllegalAccessException e) { throw new AssertionError(e); }
                }
            }:null;
            Child result=null;Throwable caught=null;
            try { result=create(route,value,marker,delegate); }catch(Throwable e) { caught=e; }
            if(failure!=0)check(caught==Trace.failure);else check(caught==null&&result==Trace.partial);
            check(Trace.partial!=null);
            check(!Trace.flags[1]&&Trace.values[1]==0&&Trace.bases[1]==0&&Trace.children[1]==0);
            check(Trace.partial.zeroFlag==mutate&&Trace.partial.flag()==mutate&&InheritedUnwrittenBooleanCaller.read(Trace.partial)==mutate);
            long expected=0;
            for(int stage=1;stage<=5;stage++) {
                if(stage==4&&!delegate)continue;
                expected=expected*10+stage;
                check(Trace.flags[stage]==(stage==1?false:mutate));
                check(Trace.values[stage]==(stage>=3?value:0));
                check(Trace.bases[stage]==(stage>=2?value+1:0));
                check(Trace.children[stage]==(stage==5?(delegate?marker:7):0));
                if(stage==failure)break;
            }
            check(Trace.events==expected);
            check(Trace.partial.baseValue==(failure==1?0:value+1));
            check(Trace.partial.value==(failure==1||failure==2?0:value));
            check(((Child)Trace.partial).childValue==(failure!=0&&failure<5?0:(delegate?marker:7)));
            if(result!=null) {
                flag.setBoolean(result,!mutate);check(result.flag()==!mutate&&InheritedUnwrittenBooleanCaller.read(result)==!mutate);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
