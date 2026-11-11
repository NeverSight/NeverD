import fixture.Child;
import fixture.Trace;
import java.net.URI;
import java.net.URISyntaxException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class InheritedProtectedAllocationHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned inherited protected allocation");}
    private static final class Override extends Child {
        protected String text(Object value){Trace.overrides++;return super.text(value);}
    }
    private static final class Trap {public String toString(){throw new AssertionError("owned inherited protected allocation");}}
    private static URI call(int route,Child receiver,Object value)throws Throwable {
        if(route==0)return receiver.make(value);if(route==1)return InheritedProtectedAllocationCaller.call(receiver,value);
        if(route==2){try{return (URI)Child.class.getMethod("make",Object.class).invoke(receiver,value);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (URI)MethodHandles.publicLookup().findVirtual(Child.class,"make",MethodType.methodType(URI.class,Object.class)).invokeExact(receiver,value);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final Child receiver=mode==11?null:operation==0?new Child():new Override();
        final Object value=mode==1?null:mode==2?new Trap():new Object();
        final String desired=mode==8?"%":mode==9?null:mode==10?"":new String((operation==0?"owned:":"other:/")+mode);
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        URI previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.overrides=0;Trace.value=sentinel;Trace.payload=sentinel;Trace.result="old:seed";int calls=Trace.calls;
            Trace.action=mode==3?null:new Runnable(){public void run(){require(Trace.events==1&&Trace.value==value&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            URI actual=null;Throwable failure=null;try{actual=call(route,receiver,value);}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+(mode==11?0:1)&&Trace.value==(mode==11?sentinel:value)&&Trace.overrides==(mode==11?0:operation));
            require(Trace.events==(mode==11?0:mode==3||mode==6?1:mode==4||mode==5?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual==null);
            else if(mode==3||mode==9||mode==11)require(failure instanceof NullPointerException&&actual==null);
            else if(mode==8)require(failure instanceof URISyntaxException&&((URISyntaxException)failure).getInput().equals(desired)&&((URISyntaxException)failure).getIndex()==0&&actual==null);
            else {require(failure==null&&actual!=null&&actual!=previous&&actual.toString().equals(desired));previous=actual;}
            require(Trace.payload==(mode==3||mode==6||mode==11?sentinel:payload));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
