import fixture.Client;
import fixture.Provider;
import fixture.Trace;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.AbstractList;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

public final class BoundedScalarTransferHarness {
    private static void require(boolean value) {
        if (!value) throw new AssertionError("owned bounded scalar transfer");
    }
    private static final class ObservedList extends AbstractList<Object> {
        Object element;
        RuntimeException failure;
        Error error;
        public int size() {throw new AssertionError("owned bounded scalar transfer inspected list");}
        public Object get(int index) {
            require(index==0);
            Trace.hit(4);
            if (failure!=null) throw failure;
            if (error!=null) throw error;
            return element;
        }
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static List call(int route, List values, int first, int second, boolean flag) throws Throwable {
        if (route==0) return Client.call(values,first,second,flag);
        if (route==1) return BoundedScalarTransferCaller.call(values,first,second,flag);
        if (route==2) {
            try {return (List)Client.class.getMethod("call",List.class,int.class,int.class,boolean.class).invoke(null,values,first,second,flag);}
            catch (InvocationTargetException failure) {throw failure.getCause();}
        }
        if (route==3) return (List)MethodHandles.publicLookup().findStatic(Client.class,"call",
            MethodType.methodType(List.class,List.class,int.class,int.class,boolean.class)).invokeExact(values,first,second,flag);
        return Client.call(values,first,second);
    }
    @SuppressWarnings("rawtypes")
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        require(Arrays.equals(Provider.class.getMethod("read",List.class,int.class,int.class,boolean.class).getExceptionTypes(),new Class[]{IOException.class}));
        require(Arrays.equals(Client.class.getMethod("call",List.class,int.class,int.class,boolean.class).getExceptionTypes(),new Class[]{IOException.class}));
        require(Arrays.equals(Client.class.getMethod("call",List.class,int.class,int.class).getExceptionTypes(),new Class[]{IOException.class}));
        final ObservedList input=new ObservedList(),output=new ObservedList();
        final List values=mode==1?null:mode==2?Collections.emptyList():input;
        final int first=Integer.MIN_VALUE+37,second=Integer.MAX_VALUE-41;
        final boolean flag=route!=4&&operation==1;
        final Object element=mode==12?new Object():Long.valueOf(917);
        input.element=output.element=element;
        final Object desired=mode==3?input:mode==11?new Object():mode==13?null:output;
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure"),readFailure=new IllegalStateException("owned read failure");
        final Error actionError=new Error("owned action error"),readError=new Error("owned read error");
        final IOException checked=new IOException("owned checked failure");
        output.failure=mode==14?readFailure:null;output.error=mode==15?readError:null;
        Trace.failure=new IllegalArgumentException("owned stage failure");
        Trace.failAt=mode==6?1:mode==7?2:0;
        for (int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();
            Trace.events=0;Trace.values=null;Trace.first=0;Trace.second=0;Trace.flag=!flag;
            Trace.payload=sentinel;Trace.result=sentinel;Trace.checked=mode==8||mode==10?checked:null;
            final int calls=Trace.calls;
            Trace.action=new Runnable() {public void run() {
                require(Trace.events==1&&Trace.values==values&&Trace.first==first&&Trace.second==second&&Trace.flag==flag&&Trace.payload==sentinel);
                Trace.hit(3);Trace.payload=payload;Trace.result=desired;
                if (mode==9) Trace.checked=checked;
                if (mode==10) Trace.checked=null;
                if (mode==4) throw actionFailure;
                if (mode==5) throw actionError;
            }};
            Object actual=sentinel,read=sentinel;Throwable failure=null;
            try {actual=call(route,values,first,second,flag);if (operation==1) read=(Number)((List)actual).get(0);}
            catch (Throwable error) {failure=error;}
            final boolean later=operation==1&&(mode<=3||mode==10||mode==12||mode>=14);
            final long events=mode==6?1:mode==4||mode==5||mode==8||mode==9?13:later?1324:132;
            require(Trace.events==events&&Trace.calls==calls+1);
            require(Trace.values==values&&Trace.first==first&&Trace.second==second&&Trace.flag==flag);
            require(Trace.payload==(mode==6?sentinel:payload));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:mode==8||mode==9?checked:null;
            if (expected!=null) require(failure==expected&&actual==sentinel&&read==sentinel);
            else if (mode==11) require(failure instanceof ClassCastException&&actual==sentinel&&read==sentinel);
            else if (operation==1&&mode==12) require(failure instanceof ClassCastException&&actual==desired&&read==sentinel);
            else if (operation==1&&mode==13) require(failure instanceof NullPointerException&&actual==null&&read==sentinel);
            else if (operation==1&&mode>=14) require(failure==(mode==14?readFailure:readError)&&actual==desired&&read==sentinel);
            else require(failure==null&&actual==desired&&read==(operation==1?element:sentinel));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
