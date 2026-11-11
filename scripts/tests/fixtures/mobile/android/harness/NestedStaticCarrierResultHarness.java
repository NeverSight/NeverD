import fixture.*;
import java.util.concurrent.atomic.AtomicReference;
import java.lang.reflect.*;
public final class NestedStaticCarrierResultHarness {
    static int beforeCalls,afterCalls,mode,attempt;
    static Throwable failure;
    static void require(boolean condition){if(!condition)throw new AssertionError("owned nested static carrier result");}
    @SuppressWarnings("unchecked") static <E extends Throwable> void raise(Throwable error)throws E{throw (E)error;}
    static class Seed {public int hashCode(){throw new AssertionError("unexpected content hash");}public String toString(){throw new AssertionError("unexpected content text");}}
    static final class OddReference extends AtomicReference<Object>{OddReference(Object value){super(value);}}
    static final class Action implements Runnable {
        final int stage;
        Action(int stage){this.stage=stage;}
        public void run(){if(stage==1)beforeCalls++;else afterCalls++;if(stage==1&&(mode==3||mode==4)||stage==2&&(mode>=5&&mode<=8)){if(mode==8)throw (RuntimeException)null;NestedStaticCarrierResultHarness.<RuntimeException>raise(failure);}}
    }
    static Class<?> nested(Method method)throws Exception{
        TypeVariable<Method>[] formal=method.getTypeParameters();require(formal.length==1&&formal[0].getGenericDeclaration().equals(method)&&formal[0].getBounds().length==1&&formal[0].getBounds()[0]==Object.class);
        ParameterizedType outer=(ParameterizedType)method.getGenericReturnType();require(outer.getRawType()==AtomicReference.class&&outer.getActualTypeArguments().length==1);ParameterizedType inner=(ParameterizedType)outer.getActualTypeArguments()[0];Class<?> slot=(Class<?>)inner.getRawType();require(slot.getDeclaringClass()==Holder.class&&Modifier.isPrivate(slot.getModifiers())&&Modifier.isStatic(slot.getModifiers()));
        require(inner.getActualTypeArguments().length==1&&inner.getActualTypeArguments()[0].equals(formal[0]));require(slot.getTypeParameters().length==1&&slot.getTypeParameters()[0].getGenericDeclaration()==slot&&!slot.getTypeParameters()[0].equals(formal[0]));
        require(Holder.class.getTypeParameters().length==1&&Holder.class.getTypeParameters()[0].getGenericDeclaration()==Holder.class&&!Holder.class.getTypeParameters()[0].equals(formal[0]));
        require(AtomicReference.class.getTypeParameters().length==1&&AtomicReference.class.getTypeParameters()[0].getBounds()[0]==Object.class);return slot;
    }
    @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);Method factory=Holder.class.getDeclaredMethod("build"),transfer=Holder.class.getDeclaredMethod("same",AtomicReference.class);transfer.setAccessible(true);
        require(Modifier.isPublic(factory.getModifiers())&&Modifier.isPrivate(transfer.getModifiers())&&Modifier.isStatic(factory.getModifiers())&&Modifier.isStatic(transfer.getModifiers()));require(nested(factory)==nested(transfer));require(transfer.getGenericReturnType().equals(transfer.getGenericParameterTypes()[0]));require(!factory.getTypeParameters()[0].equals(transfer.getTypeParameters()[0]));
        Object previousResult=null;Throwable previousError=null;
        for(attempt=0;attempt<2;attempt++){
            Trace.calls=Trace.phase=beforeCalls=afterCalls=0;Trace.seen=null;Trace.before=new Action(1);Trace.after=new Action(2);
            failure=mode==4||mode==7?new Exception("owned checked identity"):mode==6?new AssertionError("owned error identity"):new IllegalArgumentException("owned runtime identity");
            Object seed=mode==9?new Object[]{new Seed()}:new Seed();AtomicReference input=mode==1?null:mode==9?new OddReference(seed):new AtomicReference(mode==2?seed:null);Object inputValue=input==null?null:input.get();Object result=null;Throwable error=null;
            try{if(route==0)result=Client.produce();else if(route==1)result=Holder.send();else try{result=route==2?factory.invoke(null):transfer.invoke(null,input);}catch(InvocationTargetException e){throw e.getCause();}}catch(Throwable e){error=e;}
            boolean early=route!=3&&(mode==3||mode==4),late=mode>=5&&mode<=8;
            require(Trace.calls==(route==3||early?1:2)&&Trace.phase==(early?1:2));require(beforeCalls==(route==3?0:1)&&afterCalls==(early?0:1));
            if(early){require(Trace.seen==null&&result==null&&error==failure);}
            else{
                require(route==3?Trace.seen==input:Trace.seen!=null);if(Trace.seen!=null)require(Trace.seen.get()==(route==3?inputValue:null));
                if(late){require(result==null);if(mode==8)require(error!=null&&error.getClass()==NullPointerException.class&&error!=previousError);else require(error==failure);}
                else{require(error==null&&result==Trace.seen);if(route==3)require(result==input);else require(result!=previousResult);if(result!=null){AtomicReference reference=(AtomicReference)result;Object token=new Seed();reference.set(token);require(reference.get()==token);require(reference.compareAndSet(token,seed)&&reference.get()==seed);}}
            }
            previousResult=result;previousError=error;
        }
        System.out.println("attempts=2,nested-static-carrier-result=matched");
    }
}
