import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedAllocatingConstructorHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned closed allocating constructor");}
    static Core call(int route,long bits,Callback callback) throws Throwable {
        if(route==0)return Client.call(bits,callback);
        if(route==1)return ClosedAllocatingConstructorCaller.call(bits,callback);
        Constructor<Core> ctor=Core.class.getDeclaredConstructor(long.class,Callback.class);ctor.setAccessible(true);
        if(route==2)try{return ctor.newInstance(bits,callback);}catch(InvocationTargetException e){throw e.getCause();}
        return (Core)MethodHandles.lookup().unreflectConstructor(ctor).invokeExact(bits,callback);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        RuntimeException failure=new IllegalStateException("owned constructor failure");Trace.failure=failure;
        Trace.value=mode==3?null:new Value();Trace.label=mode==4?null:new Label();
        int stop=mode>=8&&mode<=13?mode-7:mode==14?8:mode==15?7:mode==1?6:mode==2?7:0;
        int[] calls={0};
        for(int attempt=0;attempt<2;attempt++){
            Trace.events=0;Trace.box=null;Trace.partial=null;
            Trace.failAt=mode>=8?stop:0;
            Value expected=mode==5?null:new Value();
            Callback<Value,Label,Value> hook=(value,label)->{require(value==Trace.value&&label==Trace.label);calls[0]++;Trace.hit(7);if(mode==2)throw failure;return expected;};
            Callback callback=mode==1?null:hook;
            long bits=mode==6?Long.MIN_VALUE:mode==7?Long.MAX_VALUE:42+attempt;
            int before=calls[0];Core result=null;Throwable caught=null;
            try{result=call(route,bits,callback);}catch(Throwable error){caught=error;}
            long events=0;
            if(mode==8&&attempt==1){require(caught instanceof NoClassDefFoundError);}
            else {
                for(int event=attempt==0?1:2;event<=8;event++){events=events*10+event;if(event==stop)break;}
                if(mode==8)require(caught instanceof ExceptionInInitializerError&&caught.getCause()==failure);
                else if(mode==1)require(caught instanceof NullPointerException);
                else if(stop!=0)require(caught==failure);
                else require(caught==null&&result==Trace.partial);
            }
            require(Trace.events==events);
            boolean entered=!(mode==8&&attempt==1),boxed=entered&&(stop==0||stop>=3),core=entered&&(stop==0||stop>=4);
            require((Trace.box!=null)==boxed&&(Trace.partial!=null)==core);
            if(boxed)require(Trace.box.bits==bits);
            if(core){
                require(Trace.partial.box==(stop==0||stop>=5?Trace.box:null));
                require(Trace.partial.callback==(stop==0||stop>=6?callback:null));
                require(Trace.partial.result==(stop==0||stop>=8?expected:null));
            }
            require(calls[0]==before+(entered&&(stop==0||stop>=7)?1:0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
