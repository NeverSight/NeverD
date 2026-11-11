import fixture.Client;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class InterfaceSeedVarargsHarness {
    private static void require(boolean value){if(!value)throw new AssertionError("owned interface seed varargs");}
    private static final class Trap implements CharSequence {public int length(){throw new AssertionError("owned interface seed varargs");}public char charAt(int i){throw new AssertionError("owned interface seed varargs");}public CharSequence subSequence(int a,int b){throw new AssertionError("owned interface seed varargs");}public String toString(){throw new AssertionError("owned interface seed varargs");}}
    private static CharSequence call(int route,CharSequence text,String message,Object[] arguments)throws Throwable {
        if(route==0)return Client.call(text,message,arguments);if(route==1)return InterfaceSeedVarargsCaller.call(text,message,arguments);
        if(route==2){try{return (CharSequence)Client.class.getMethod("call",CharSequence.class,String.class,Object[].class).invoke(null,text,message,arguments);}catch(InvocationTargetException failure){throw failure.getCause();}}
        return (CharSequence)MethodHandles.publicLookup().findStatic(Client.class,"call",MethodType.methodType(CharSequence.class,CharSequence.class,String.class,Object[].class)).invokeExact(text,message,arguments);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final CharSequence text=mode==1?null:operation==0?new StringBuilder("owned input"):new Trap();
        final String message=mode==3?null:new String(" owned message ");
        final Object[] arguments=mode==2?null:mode==10?new Object[0]:operation==1&&mode!=11?new String[3]:new Object[3];
        final CharSequence desired=mode==8?new StringBuilder("other result"):mode==9?null:new String("owned result");
        final RuntimeException actionFailure=new IllegalArgumentException("owned action failure");final Error actionError=new Error("owned action error");
        Trace.failure=new IllegalArgumentException("owned stage failure");Trace.failAt=mode==6?1:mode==7?2:0;
        for(int attempt=0;attempt<2;attempt++) {
            final String oldElement=new String("old"),marker=new String("written");if(arguments!=null&&arguments.length>0){arguments[0]=oldElement;arguments[1]=mode==11?arguments:oldElement;arguments[2]=null;}
            final Object sentinel=new Object(),payload=new Object();Trace.events=0;Trace.text=null;Trace.message=null;Trace.arguments=null;Trace.payload=sentinel;Trace.result="original result";int calls=Trace.calls;
            Trace.action=new Runnable(){public void run(){require(Trace.events==1&&Trace.text==text&&Trace.message==message&&Trace.arguments==arguments&&Trace.payload==sentinel);Trace.hit(3);Trace.payload=payload;Trace.result=desired;if(arguments!=null&&arguments.length>0)arguments[0]=marker;if(mode==4)throw actionFailure;if(mode==5)throw actionError;}};
            CharSequence actual="original result";int length=42;Throwable failure=null;try{actual=call(route,text,message,arguments);length=((String)actual).length();}catch(Throwable error){failure=error;}
            require(Trace.calls==calls+1&&Trace.text==text&&Trace.message==message&&Trace.arguments==arguments&&Trace.events==(mode==6?1:mode==4||mode==5?13:132));
            Throwable expected=mode==4?actionFailure:mode==5?actionError:mode==6||mode==7?Trace.failure:null;
            if(expected!=null)require(failure==expected&&actual=="original result"&&length==42);
            else if(mode==8)require(failure instanceof ClassCastException&&actual==desired&&length==42);
            else if(mode==9)require(failure instanceof NullPointerException&&actual==null&&length==42);
            else require(failure==null&&actual==desired&&length==((String)desired).length());
            require(Trace.payload==(mode==6?sentinel:payload));
            if(arguments!=null&&arguments.length>0)require(arguments[0]==(mode==6?oldElement:marker)&&arguments[1]==(mode==11?arguments:oldElement)&&arguments[2]==null);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
