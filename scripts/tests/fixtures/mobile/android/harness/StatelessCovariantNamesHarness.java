import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class StatelessCovariantNamesHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned covariant names"); }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Core core=new Core();Trace.failure=new RuntimeException("owned naming failure");
        Trace.failAt=mode>=1&&mode<=4?mode:0;
        long[] bits={0,-1,Long.MIN_VALUE,Long.MAX_VALUE,0x0123456789abcdefL,0x7ff0000000000001L};
        Object sentinel=new Object(),identity=null;
        for(int attempt=0;attempt<2;attempt++) {
            Narrow reply=mode==5?null:new Narrow(attempt+17);Trace.reply=reply;
            Trace.events=0;Trace.receiver=sentinel;Trace.wide=~bits[(mode+attempt)%bits.length];
            int before=Trace.calls;Value result=null;Throwable failure=null;
            try {
                long wide=bits[(mode+attempt)%bits.length];
                if(route==0)result=core.call(wide);
                else if(route==1)result=StatelessCovariantNamesCaller.call(core,wide);
                else if(route==2)result=(Value)Core.class.getMethod("call",long.class).invoke(core,wide);
                else result=(Value)MethodHandles.publicLookup().findVirtual(Core.class,"call",MethodType.methodType(Value.class,long.class)).invoke(core,wide);
            } catch(InvocationTargetException error) { failure=error.getCause(); }
              catch(Throwable error) { failure=error; }
            String steps=mode==3?"3":mode==1?"31":mode==2?"312":"3124";
            require(Trace.events==Long.parseLong(steps));boolean entered=mode!=1&&mode!=3;
            require(Trace.calls==before+(entered?1:0));
            if(entered) {
                require(Trace.receiver!=null&&Trace.receiver!=sentinel&&Trace.wide==bits[(mode+attempt)%bits.length]);
                if(identity==null)identity=Trace.receiver;else require(identity==Trace.receiver);
            } else require(Trace.receiver==sentinel);
            if(mode>=1&&mode<=4)require(failure==Trace.failure);
            else require(failure==null&&result==reply);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
