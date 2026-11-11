import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class PrefixLocalAllocationHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned prefix local allocation");}
    static Core make(int route,Object input,long word,String label)throws Throwable {
        if(route==0)return new Core(input,word,label);
        if(route==1)return PrefixLocalAllocationCaller.make(input,word,label);
        if(route==2){try{return(Core)Core.class.getConstructor(Object.class,long.class,String.class).newInstance(input,word,label);}catch(InvocationTargetException f){throw f.getCause();}}
        return(Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Object.class,long.class,String.class)).invokeExact(input,word,label);
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        long[] words={Long.MIN_VALUE,Long.MAX_VALUE,0x12345678abcdef01L,-0x12345678abcdef0L};
        for(int attempt=0;attempt<2;attempt++) {
            Object input=mode==1?null:new Object();String label=mode==2?null:new String("owned local label");long word=words[(mode+attempt)%words.length];
            Trace.events=Trace.word=0;Trace.partial=Trace.input=null;Trace.local=null;Trace.failAt=mode>=3&&mode<=7?mode-2:0;Trace.failure=new RuntimeException("owned local failure");
            Throwable caught=null;Core actual=null;try{actual=make(route,input,word,label);}catch(Throwable f){caught=f;}
            long expected=Trace.failAt==1?1:Trace.failAt==2?12:Trace.failAt==3?123:Trace.failAt==4?1234:12345;
            require(Trace.events==expected&&Trace.input==(Trace.failAt==1?null:input));
            if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null&&actual==Trace.partial);
            if(Trace.failAt==1||Trace.failAt==2)require(Trace.partial==null);
            else {Core partial=(Core)Trace.partial;require(partial!=null&&partial.label==((Trace.failAt==3||Trace.failAt==4)?null:label));
                require((partial.value==null)==(Trace.failAt==3));
                if(partial.value!=null){require(partial.value.read()==input&&Trace.word==word&&Trace.local==partial.value);}
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
