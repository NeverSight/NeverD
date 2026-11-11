import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class PureAllocationReceiverCopyHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned pure allocation receiver copy"); }
    static Target call(int route,long bits,boolean enabled,int tag,boolean active) throws Throwable {
        if(route==0)return Client.make(bits,enabled,tag,active);
        if(route==1)return PureAllocationReceiverCopyCaller.call(bits,enabled,tag,active);
        if(route==2)try { return (Target)Client.class.getDeclaredMethod("make",long.class,boolean.class,int.class,boolean.class).invoke(null,bits,enabled,tag,active); }catch(InvocationTargetException error) { throw error.getCause(); }
        return (Target)MethodHandles.publicLookup().findStatic(Client.class,"make",MethodType.methodType(Target.class,long.class,boolean.class,int.class,boolean.class)).invokeExact(bits,enabled,tag,active);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++) {
            long bits=mode==1?Long.MIN_VALUE:mode==2?Long.MAX_VALUE:mode==3?0:0x8123456789abcdefL+attempt;
            boolean enabled=mode==2||mode!=1&&mode!=3,active=mode==1||mode==2;
            int tag=mode==1?Integer.MIN_VALUE:mode==2?Integer.MAX_VALUE:mode==3?0:0x71234567+attempt;
            Trace.events=Trace.count=0;Trace.created=null;Trace.failAt=mode>=4?mode-3:0;Trace.failure=new RuntimeException("owned failure");
            Target result=null;Throwable caught=null;
            try { result=call(route,bits,enabled,tag,active); }catch(Throwable error) { caught=error; }
            require(Trace.count==1&&Trace.created!=null);
            if(Trace.failAt==0)require(caught==null&&result==Trace.created);else require(caught==Trace.failure&&result==null);
            int length=Trace.failAt==0?5:Trace.failAt,events=0;for(int i=1;i<=length;i++)events=events*10+i;
            require(Trace.events==events);
            require(Trace.created.bits==(length>=2?bits:0)&&Trace.created.enabled==(length>=3?enabled:false)&&Trace.created.tag==(length>=4?tag:0)&&Trace.created.active==(length>=5?active:false));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
