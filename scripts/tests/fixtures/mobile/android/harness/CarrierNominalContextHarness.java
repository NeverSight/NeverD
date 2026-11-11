import fixture.Client;
import fixture.Trace;
import fixture.Failure;
import java.io.File;
import java.util.Collection;
import java.util.ArrayList;
import java.util.AbstractCollection;
import java.util.Collections;
import java.util.Iterator;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class CarrierNominalContextHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned carrier nominal context");}
    private static final class Result extends File {
        final RuntimeException failure;
        Result(RuntimeException failure){super("owned/./result");this.failure=failure;}
        @Override public String getName(){if(failure!=null)throw failure;return "owned";}
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    private static File call(int route,int operation,Collection values,File file,String text,boolean flag)throws Throwable {
        if(route==0)return operation==0?Client.call(values,file,text):Client.full(values,file,text,flag);
        if(route==1)return operation==0?CarrierNominalContextCaller.call(values,file,text):CarrierNominalContextCaller.full(values,file,text,flag);
        if(route==2){try{return operation==0?(File)Client.class.getMethod("call",Collection.class,File.class,String.class).invoke(null,values,file,text):(File)Client.class.getMethod("full",Collection.class,File.class,String.class,boolean.class).invoke(null,values,file,text,flag);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (File)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(File.class,Collection.class,File.class,String.class)).invokeExact(values,file,text);
        return (File)MethodHandles.publicLookup().findStatic(Client.class,"full",MethodType.methodType(File.class,Collection.class,File.class,String.class,boolean.class)).invokeExact(values,file,text,flag);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Collection values=mode==1?null:mode==10?new AbstractCollection(){public int size(){throw new AssertionError("owned carrier nominal context");}public Iterator iterator(){throw new AssertionError("owned carrier nominal context");}}:mode==11?Collections.unmodifiableCollection(new ArrayList<Object>()):new ArrayList<Object>();
        final File file=mode==2?null:new File("owned/./input");final String text=mode==3?null:new String(" owned-"+mode+" ");final boolean flag=mode%2==0;
        final Failure actionFailure=new Failure("owned action failure"),lateFailure=new Failure("owned result failure");final Error actionError=new Error("owned action error");
        final File desired=mode==9?null:new Result(mode==8?lateFailure:null),initial=new File("owned/initial");
        Trace.failure=new Failure("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++){
            if(values!=null&&mode!=10&&mode!=11)values.clear();
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.values=null;Trace.file=initial;Trace.text="sentinel";Trace.flag=!(operation==0?false:flag);Trace.payload=sentinel;Trace.result=initial;int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.values==values&&Trace.file==file&&Trace.text==text&&Trace.flag==(operation==0?false:flag)&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(values!=null&&mode!=10)values.add(payload);if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            File actual=initial;String name=null;Throwable failure=null;try{actual=call(route,operation,values,file,text,flag);name=actual.getName();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.values==values&&Trace.file==file&&Trace.text==text&&Trace.flag==(operation==0?false:flag)&&Trace.events==(mode==6?1:mode==4||mode==5||mode==11?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==initial&&name==null);
            else if(mode==11)require(failure instanceof UnsupportedOperationException&&actual==initial&&name==null);
            else if(mode==8)require(failure==lateFailure&&actual==desired&&name==null);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&name==null);
            else require(failure==null&&actual==desired&&"owned".equals(name));
            require(Trace.payload==(mode==6?sentinel:payload)&&Trace.result==(mode==6?initial:desired));
            if(values!=null&&mode!=10)require(values.size()==(mode==6||mode==11?0:1));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
