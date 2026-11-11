import fixture.Client;
import fixture.Provider;
import fixture.Trace;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.nio.ByteBuffer;
import java.util.AbstractList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

public final class TokenCarrierThrowsHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned token carrier throws");
    }
    private static final class ObservedList extends AbstractList<Object> {
        Object element;
        RuntimeException failure;
        Error error;
        public int size() {throw new AssertionError("owned token carrier throws inspected list");}
        public Object get(int index) {
            require(index==0);
            Trace.hit(4);
            if (failure!=null) throw failure;
            if (error!=null) throw error;
            return element;
        }
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static List call(int route, ByteBuffer context, Class token) throws Throwable {
        if (route==0) return Client.call(context,token);
        if (route==1) return TokenCarrierThrowsCaller.call(context,token);
        if (route==2) {
            try {return (List)Client.class.getMethod("call",ByteBuffer.class,Class.class).invoke(null,context,token);}
            catch (InvocationTargetException failure) {throw failure.getCause();}
        }
        return (List)MethodHandles.publicLookup().findStatic(Client.class,"call",
            MethodType.methodType(List.class,ByteBuffer.class,Class.class)).invokeExact(context,token);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        require(Arrays.equals(Provider.class.getMethod("read",ByteBuffer.class,Class.class).getExceptionTypes(),new Class[]{IOException.class}));
        require(Arrays.equals(Client.class.getMethod("call",ByteBuffer.class,Class.class).getExceptionTypes(),new Class[]{IOException.class}));
        final ByteBuffer context=mode==1?null:ByteBuffer.allocate(7);
        final Class token=mode==2?null:mode==3?Integer.class:String.class;
        final ObservedList output=new ObservedList();
        final Object element=mode==12?new Object():new String("owned opaque value");
        output.element=element;
        final Object desired=mode==11?new Object():mode==13?null:mode==16?Collections.emptyList():output;
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure"),readFailure=new IllegalStateException("owned read failure");
        final Error actionError=new Error("owned action error"),readError=new Error("owned read error");
        final IOException checked=new IOException("owned checked failure");
        output.failure=mode==14?readFailure:null;output.error=mode==15?readError:null;
        Trace.failure=new IllegalArgumentException("owned stage failure");
        Trace.failAt=mode==6?1:mode==7?2:0;
        for (int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();
            Trace.events=0;Trace.context=null;Trace.token=null;
            Trace.payload=sentinel;Trace.result=sentinel;Trace.checked=mode==8||mode==10?checked:null;
            if (context!=null) context.position(0);
            final int calls=Trace.calls;
            Trace.action=new Runnable() {public void run() {
                require(Trace.events==1&&Trace.context==context&&Trace.token==token&&Trace.payload==sentinel);
                Trace.hit(3);Trace.payload=payload;Trace.result=desired;
                if (context!=null) context.position(3);
                if (mode==9) Trace.checked=checked;
                if (mode==10) Trace.checked=null;
                if (mode==4) throw actionFailure;
                if (mode==5) throw actionError;
            }};
            Trace.after=new Runnable() {public void run() {
                require(Trace.events==132&&Trace.context==context&&Trace.token==token&&Trace.result==desired);
                if (mode==17) Trace.result=sentinel;
            }};
            Object actual=sentinel,read=sentinel;Throwable failure=null;
            try {actual=call(route,context,token);if (operation==1) read=(String)((List)actual).get(0);}
            catch (Throwable error) {failure=error;}
            final boolean later=operation==1&&(mode<=3||mode==10||mode==12||mode==14||mode==15||mode==17);
            final long events=mode==6?1:mode==4||mode==5||mode==8||mode==9?13:later?1324:132;
            require(Trace.events==events&&Trace.calls==calls+1);
            require(Trace.context==context&&Trace.token==token);
            require(context==null||context.position()==(mode==6?0:3));
            require(Trace.payload==(mode==6?sentinel:payload));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==8||mode==9?checked:null;
            if (expected!=null) require(failure==expected&&actual==sentinel&&read==sentinel);
            else if (mode==11) require(failure instanceof ClassCastException&&actual==sentinel&&read==sentinel);
            else if (operation==1&&mode==12) require(failure instanceof ClassCastException&&actual==desired&&read==sentinel);
            else if (operation==1&&mode==13) require(failure instanceof NullPointerException&&actual==null&&read==sentinel);
            else if (operation==1&&(mode==14||mode==15)) require(failure==(mode==14?readFailure:readError)&&actual==desired&&read==sentinel);
            else if (operation==1&&mode==16) require(failure instanceof IndexOutOfBoundsException&&actual==desired&&read==sentinel);
            else require(failure==null&&actual==desired&&read==(operation==1?element:sentinel));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
