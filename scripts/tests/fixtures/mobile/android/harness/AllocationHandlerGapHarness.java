import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class AllocationHandlerGapHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned allocation handler gap");}
    static Node call(int route,Object value,Object b,Object c,Object d,Object e,Object last)throws Throwable{
        if(route==0)return Client.make(value,b,c,d,e,last);if(route==1)return AllocationHandlerGapCaller.make(value,b,c,d,e,last);
        if(route==2){try{return(Node)Client.class.getMethod("make",Object.class,Object.class,Object.class,Object.class,Object.class,Object.class).invoke(null,value,b,c,d,e,last);}catch(InvocationTargetException f){throw f.getCause();}}
        return(Node)MethodHandles.publicLookup().findStatic(Client.class,"make",MethodType.methodType(Node.class,Object.class,Object.class,Object.class,Object.class,Object.class,Object.class)).invokeExact(value,b,c,d,e,last);
    }
    public static void main(String[]args)throws Throwable{
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++){
            Object value=mode==1?null:new Object();Trace.partial=null;Trace.caught=null;Trace.events=0;Trace.arguments=null;
            Object b=new Object(),c=null,d=new String("owned"),e=new Object(),last=value;Object[] expectedArgs={value,b,c,d,e,last};
            Trace.failAt=mode==2||mode==4?1:mode==3||mode==5?2:0;
            Trace.failure=mode>=4?new IllegalStateException("owned uncaught"):new IllegalArgumentException("owned caught");
            Node actual=null;Throwable failure=null;try{actual=call(route,value,b,c,d,e,last);}catch(Throwable f){failure=f;}
            Node partial=(Node)Trace.partial;require(partial!=null && partial.value==((mode==2||mode==4)?null:value));
            if(mode==2||mode==3)require(actual==null && failure==null && Trace.caught==Trace.failure);
            else if(mode==4||mode==5)require(actual==null && failure==Trace.failure && Trace.caught==null);
            else require(failure==null && actual==partial && Trace.caught==null);
            if(mode==2||mode==3){require(Trace.arguments!=null);for(int i=0;i<6;i++)require(Trace.arguments[i]==expectedArgs[i]);}
            else require(Trace.arguments==null);
            if(mode!=2 && mode!=4){require(partial.arguments!=null);for(int i=0;i<6;i++)require(partial.arguments[i]==expectedArgs[i]);}
            else require(partial.arguments==null);
            long expected=mode==2?13:mode==3?123:mode==4?1:12;require(Trace.events==expected);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
