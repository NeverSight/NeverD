import fixture.Client;
import fixture.Provider;
import fixture.Trace;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.security.Key;
import java.util.Arrays;

public final class InstanceBoundTokenHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned interface token context instance");
    }
    private static final class TrapKey implements Key {
        public String getAlgorithm() {throw new AssertionError("opaque context inspected");}
        public String getFormat() {throw new AssertionError("opaque context inspected");}
        public byte[] getEncoded() {throw new AssertionError("opaque context inspected");}
    }
    public static final class FutureProvider extends Provider {
        public <T extends CharSequence> T read(Key context, Class<T> type) throws IOException {
            Trace.hit(9);
            return super.read(context,type);
        }
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static CharSequence call(int route, Provider receiver, Key context, Class type) throws Throwable {
        if (route==0) return Client.call(receiver,context,type);
        if (route==1) return InstanceBoundTokenCaller.call(receiver,context,type);
        if (route==2) {
            try {return (CharSequence)Client.class.getMethod("call",Provider.class,Key.class,Class.class).invoke(null,receiver,context,type);}
            catch (InvocationTargetException failure) {throw failure.getCause();}
        }
        return (CharSequence)MethodHandles.publicLookup().findStatic(Client.class,"call",
            MethodType.methodType(CharSequence.class,Provider.class,Key.class,Class.class)).invokeExact(receiver,context,type);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        require(Arrays.equals(Provider.class.getMethod("read",Key.class,Class.class).getExceptionTypes(),new Class[]{IOException.class}));
        require(Arrays.equals(Client.class.getMethod("call",Provider.class,Key.class,Class.class).getExceptionTypes(),new Class[]{IOException.class}));
        final Provider receiver=mode==15?null:mode==14?new FutureProvider():new Provider();
        final Key context=mode==1?null:new TrapKey();
        final Class type=mode==2?null:mode==3?Integer.class:String.class;
        final Object desired=mode==11?new Object():mode==12?new StringBuilder("result"):mode==13?null:new String("result");
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");
        final Error actionError=new Error("owned action error");
        final IOException checked=new IOException("owned checked failure");
        Trace.failure=new IllegalArgumentException("owned stage failure");
        Trace.failAt=mode==6?1:mode==7?2:0;
        for (int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();
            Trace.events=0;Trace.type=null;Trace.context=null;Trace.receiver=null;
            Trace.payload=sentinel;Trace.result=sentinel;
            Trace.checked=mode==8||mode==10?checked:null;
            final int calls=Trace.calls;
            Trace.action=new Runnable() {public void run() {
                require(Trace.events==(mode==14?91:1)&&Trace.type==type&&Trace.context==context&&Trace.receiver==receiver&&Trace.payload==sentinel);
                Trace.hit(3);Trace.payload=payload;Trace.result=desired;
                if (mode==9) Trace.checked=checked;
                if (mode==10) Trace.checked=null;
                if (mode==4) throw actionFailure;
                if (mode==5) throw actionError;
            }};
            Object actual=sentinel;int value=42;Throwable failure=null;
            try {actual=call(route,receiver,context,type);if (operation==1) value=((String)actual).length();}
            catch (Throwable error) {failure=error;}
            final long events=mode==15?0:mode==14?9132:mode==6?1:mode==4||mode==5||mode==8||mode==9?13:132;
            require(Trace.events==events&&Trace.calls==calls+(mode==15?0:1));
            require(Trace.receiver==receiver&&Trace.context==(mode==15?null:context)&&Trace.type==(mode==15?null:type));
            require(Trace.payload==(mode==6||mode==15?sentinel:payload));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==8||mode==9?checked:null;
            if (expected!=null) require(failure==expected&&actual==sentinel&&value==42);
            else if (mode==15) require(failure instanceof NullPointerException&&actual==sentinel&&value==42);
            else if (mode==11) require(failure instanceof ClassCastException&&actual==sentinel&&value==42);
            else if (operation==1&&mode==12) require(failure instanceof ClassCastException&&actual==desired&&value==42);
            else if (operation==1&&mode==13) require(failure instanceof NullPointerException&&actual==null&&value==42);
            else require(failure==null&&actual==desired&&value==(operation==1?6:42));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
