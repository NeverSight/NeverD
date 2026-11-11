import fixture.*;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ArrayNominalContextHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned array nominal context");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static String call(int route,int operation,Object values,CharSequence delimiter)throws Throwable {
        if(route==0)return operation==0?Core.call((Object[])values,delimiter):Core.checked(values,delimiter);
        if(route==1)return operation==0?ArrayNominalContextCaller.call((Object[])values,delimiter):ArrayNominalContextCaller.checked(values,delimiter);
        if(route==2){try{return (String)Core.class.getMethod(operation==0?"call":"checked",operation==0?Object[].class:Object.class,CharSequence.class).invoke(null,values,delimiter);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (String)MethodHandles.publicLookup().findStatic(Core.class,"call",MethodType.methodType(String.class,Object[].class,CharSequence.class)).invokeExact((Object[])values,delimiter);
        return (String)MethodHandles.publicLookup().findStatic(Core.class,"checked",MethodType.methodType(String.class,Object.class,CharSequence.class)).invokeExact(values,delimiter);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new IllegalArgumentException("owned stage");
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();final String desired=mode==3?null:new String("owned-result"),elementText=new String("owned-element"),delimiterText=new String("owned-delimiter");
            final Object element=new Object(){public String toString(){Trace.elements++;require(Trace.events==12&&Trace.payload==payload);if(mode==8)ArrayNominalContextHarness.<RuntimeException>fail(checked);return elementText;}};
            final CharSequence delimiter=new CharSequence(){public int length(){return 0;}public char charAt(int index){throw new AssertionError("owned unused charAt");}public CharSequence subSequence(int first,int last){throw new AssertionError("owned unused subSequence");}public String toString(){Trace.delimiters++;require(Trace.events==123&&Trace.payload==payload);if(mode==9)throw error;return delimiterText;}};
            final Object[] values=mode==1?null:mode==2?new Object[0]:mode==7?new String[]{"owned-initial"}:new Object[]{sentinel};
            final Object input=mode==6&&operation==1?new Object():values;Trace.events=0;Trace.values=null;Trace.delimiter=null;Trace.prefix="owned-prefix";Trace.suffix="owned-suffix";Trace.element=null;Trace.text=null;Trace.delimiterText=null;Trace.result="owned-old-result";Trace.payload=sentinel;Trace.failAt=mode==10?3:mode==11?4:0;
            final int actions=Trace.actions,elements=Trace.elements,delimiters=Trace.delimiters;
            Trace.action=new Runnable(){public void run(){Trace.actions++;require(Trace.events==1&&Trace.values==values&&Trace.delimiter==delimiter&&Trace.prefix==null&&Trace.suffix==null&&Trace.element==null&&Trace.payload==sentinel);Trace.payload=payload;Trace.result=desired;if(mode==4)throw runtime;if(mode==5)throw error;if(values!=null&&values.length!=0)values[0]=element;}};
            String result="owned-sentinel";Throwable failure=null;try{result=call(route,operation,input,delimiter);require(result.length()==12);}catch(Throwable caught){failure=caught;}
            final boolean bad=mode==6&&operation==1,action=!bad,converted=action&&mode!=4&&mode!=5&&mode!=7,read=converted&&values!=null&&values.length!=0;
            final long events=bad?0:mode==4||mode==5||mode==7?1:mode==8?12:mode==9||mode==10?123:1234;
            require(Trace.events==events&&Trace.actions==actions+(action?1:0)&&Trace.elements==elements+(read?1:0)&&Trace.delimiters==delimiters+(converted&&mode!=8&&mode!=10?1:0));
            Throwable wanted=mode==4?runtime:mode==5||mode==9?error:mode==8?checked:mode==10||mode==11?Trace.failure:null;
            if(wanted!=null)require(failure==wanted&&result.equals("owned-sentinel"));else if(bad)require(failure instanceof ClassCastException);else if(mode==7)require(failure instanceof ArrayStoreException);else if(mode==3)require(failure instanceof NullPointerException&&result==null);else require(failure==null&&result==desired);
            require(Trace.values==(action?values:null)&&Trace.delimiter==(action?delimiter:null)&&Trace.payload==(action?payload:sentinel)&&Trace.element==(read?element:null));
            require(Trace.text==(read&&mode!=8?elementText:null)&&Trace.delimiterText==(converted&&mode!=8&&mode!=10&&mode!=9?delimiterText:null));
            if(mode==7)require(values instanceof String[]&&values[0].equals("owned-initial"));else if(read)require(values[0]==element);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
