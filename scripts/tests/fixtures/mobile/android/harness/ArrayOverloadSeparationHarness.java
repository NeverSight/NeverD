import fixture.Client;
import fixture.Child;
import fixture.Trace;
import java.io.File;
import java.util.*;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ArrayOverloadSeparationHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned array overload separation");}
    @SuppressWarnings("unchecked")
    private static Child call(int route,boolean array,String[] data,Collection<File> values)throws Throwable {
        if(route==0)return array?Client.call(data,values):Client.fresh(values);if(route==1)return ArrayOverloadSeparationCaller.call(array,data,values);
        if(route==2){try{return (Child)(array?Client.class.getMethod("call",String[].class,Collection.class).invoke(null,data,values):Client.class.getMethod("fresh",Collection.class).invoke(null,values));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(array)return (Child)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(Child.class,String[].class,Collection.class)).invokeExact(data,values);
        return (Child)MethodHandles.publicLookup().findStatic(Client.class,"fresh",MethodType.methodType(Child.class,Collection.class)).invokeExact(values);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final String[] data=mode==3?null:mode==2?new String[0]:new String[]{"head","tail"};
        final Collection<File> values=mode==1?null:mode==2?Collections.<File>emptyList():Collections.singletonList(new File("owned-value"));
        final RuntimeException runtime=new IllegalArgumentException("owned action failure");final Error error=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            if(data!=null&&data.length!=0)data[0]="head";
            final Object sentinel=new Object(),payload=new Object(),desired=new Object();final String[][] selected={null};
            Trace.events=0;Trace.values=null;Trace.data=null;Trace.payload=sentinel;Trace.result=sentinel;int calls=Trace.calls,siblings=Trace.siblings;
            Trace.action=mode==10?null:new Runnable(){public void run(){require(Trace.events==1&&Trace.values==null&&Trace.payload==sentinel);require(operation==1?Trace.data==data:Trace.data!=null&&Trace.data.length==0);selected[0]=Trace.data;Trace.hit(5);Trace.payload=payload;if(Trace.data!=null&&Trace.data.length!=0)Trace.data[0]="changed";if(mode==4)throw runtime;if(mode==5)throw error;}};
            Trace.childAction=mode==11?null:new Runnable(){public void run(){require(Trace.events==1523&&Trace.values==values&&Trace.payload==payload);Trace.hit(6);Trace.result=desired;}};
            Child result=null;Throwable failure=null;try{result=call(route,operation==1,data,values);}catch(Throwable caught){failure=caught;}
            long expectedEvents=mode==6?1:mode==4||mode==5?15:mode==7?152:mode==8||mode==11?1523:mode==9?152364:mode==10?1:152364;
            require(Trace.calls==calls+1&&Trace.siblings==siblings&&Trace.events==expectedEvents);
            require(operation==1?Trace.data==data:Trace.data!=null&&Trace.data.length==0);
            Throwable expected=mode==4?runtime:mode==5?error:mode>=6&&mode<=9?Trace.failure:null;
            if(expected!=null)require(failure==expected&&result==null);
            else if(mode==10||mode==11)require(failure instanceof NullPointerException&&result==null);
            else require(failure==null&&result!=null&&result.value==desired);
            boolean afterAction=mode!=6&&mode!=10;boolean afterChild=mode!=4&&mode!=5&&mode!=6&&mode!=7&&mode!=10;
            require(Trace.payload==(afterAction?payload:sentinel));require(Trace.values==(afterChild?values:null));
            require(Trace.result==(mode==9||failure==null?desired:sentinel));
            if(operation==1&&data!=null&&data.length!=0)require(data[0].equals(afterAction?"changed":"head")&&data[1].equals("tail"));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
