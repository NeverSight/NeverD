import fixture.*;
import java.lang.invoke.*;import java.lang.reflect.*;
public final class ThisAllocationPrefixHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned this allocation prefix");}
    static Core make(int route,Object input,long word,String label)throws Throwable{
        if(route==0)return Client.make(input,word,label);if(route==1)return ThisAllocationPrefixCaller.make(input,word,label);
        if(route==2){try{return(Core)Core.class.getConstructor(Object.class,long.class,String.class).newInstance(input,word,label);}catch(InvocationTargetException f){throw f.getCause();}}
        return(Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,long.class,String.class)).invokeExact(input,word,label);
    }
    public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++){
            Object input=mode==1?null:new Object();String label=mode==2?null:new String("owned label");long word=mode==8?Long.MIN_VALUE:mode==9?Long.MAX_VALUE:0x1234567887654321L+attempt;
            Trace.partial=null;Trace.token=null;Trace.events=0;Trace.failAt=mode>=3&&mode<=7?mode-2:0;Trace.failure=new RuntimeException("owned partial failure");
            Core result=null;Throwable caught=null;try{result=make(route,input,word,label);}catch(Throwable f){caught=f;}
            if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null && result==Trace.partial);
            long expected=mode==3?1:mode==4?12:mode==5?123:mode==6?1234:12345;require(Trace.events==expected && Trace.token!=null);
            require(Trace.token.input==(mode==3?null:input) && Trace.token.word==(mode==3?0:word));
            if(mode==3||mode==4)require(Trace.partial==null);else require(Trace.partial!=null && Trace.partial.value==(mode==5?null:Trace.token) && Trace.partial.label==((mode==5||mode==6)?null:label));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
