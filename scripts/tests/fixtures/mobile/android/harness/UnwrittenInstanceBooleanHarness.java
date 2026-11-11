import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class UnwrittenInstanceBooleanHarness {
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned unwritten instance Boolean behavior"); }
    static Owner create(int route,long value,int count) throws Throwable {
        if(route==0)return Client.create(value,count);
        if(route==1)return UnwrittenInstanceBooleanCaller.create(value,count);
        if(route==2) {
            try { return Owner.class.getConstructor(long.class,int.class).newInstance(value,count); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Owner.class,MethodType.methodType(void.class,long.class,int.class));
        return (Owner)h.invokeExact(value,count);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean encoded=Boolean.parseBoolean(args[2]);
        Field flag=Owner.class.getField("zeroFlag");check(Modifier.isFinal(flag.getModifiers())==encoded);flag.setAccessible(true);
        for(int attempt=0;attempt<2;attempt++) {
            long value=0x8123456789abcdefL+attempt;int count=mode==0?0:mode==1?1:3;
            boolean mutate=mode>=6;
            Trace.events=0;Trace.partial=null;Trace.flags=new boolean[5];Trace.values=new long[5];
            Trace.failAt=mode>=2&&mode<=5?mode-1:mode==8?3:0;
            Trace.failure=new RuntimeException("owned failure");
            Trace.hook=mutate?new Hook() {
                public void observe(Owner owner,int stage) {
                    if(stage==1)try { flag.setBoolean(owner,true); }catch(IllegalAccessException e) { throw new AssertionError(e); }
                }
            }:null;
            Owner result=null;Throwable caught=null;
            try { result=create(route,value,count); }catch(Throwable e) { caught=e; }
            if(Trace.failAt!=0)check(caught==Trace.failure);else check(caught==null&&result==Trace.partial);
            check(Trace.partial!=null);check(!Trace.flags[1]&&Trace.values[1]==0);
            check(Trace.partial.zeroFlag==mutate&&Trace.partial.flag()==mutate&&UnwrittenInstanceBooleanCaller.read(Trace.partial)==mutate);
            long expected=1;
            if(Trace.failAt!=1) {
                expected=12;check(Trace.flags[2]==mutate&&Trace.values[2]==value);
                if(Trace.failAt!=2) {
                    for(int i=0;i<count;i++) { expected=expected*10+3;if(Trace.failAt==3)break; }
                    if(Trace.failAt!=3)expected=expected*10+4;
                }
            }
            check(Trace.events==expected);check(Trace.partial.value==(Trace.failAt==1?0:value));
            if(result!=null) {
                flag.setBoolean(result,!mutate);check(result.flag()==!mutate&&UnwrittenInstanceBooleanCaller.read(result)==!mutate);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
