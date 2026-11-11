import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class RawZeroReferenceHarness {
    static void require(boolean value) { if(!value)throw new AssertionError("owned raw zero reference"); }
    static Object call(int route) throws Throwable {
        Core core=new Core();
        if(route==0)return core.get();
        if(route==1)return RawZeroReferenceCaller.call(core);
        if(route==2) {
            Method bridge=null;for(Method m:Core.class.getDeclaredMethods())if(m.getName().equals("get")&&m.getReturnType()==Object.class)bridge=m;
            try{return bridge.invoke(core);}catch(InvocationTargetException e){throw e.getCause();}
        }
        return (Object)MethodHandles.publicLookup().findVirtual(Producer.class,"get",MethodType.methodType(Object.class)).invokeExact((Producer)core);
    }
    static void metadata(boolean voidResult) throws Throwable {
        Class<?> c=Class.forName("fixture.Core",false,RawZeroReferenceHarness.class.getClassLoader());
        require(c.getTypeParameters().length==0 && c.getGenericInterfaces().length==1 && c.getGenericInterfaces()[0]==Producer.class);
        TypeVariable<?> formal=Producer.class.getTypeParameters()[0];require(formal.getBounds()[0]==Object.class && Producer.class.getMethod("get").getGenericReturnType().equals(formal));
        int typed=0,erased=0;
        for(Method m:c.getDeclaredMethods())if(m.getName().equals("get")) {
            require(m.getParameterCount()==0 && m.getTypeParameters().length==0 && m.getExceptionTypes().length==0);
            if(m.getReturnType()==Object.class){require(m.isBridge()&&m.isSynthetic()&&Modifier.isPublic(m.getModifiers()));erased++;}
            else {require(m.getReturnType()==(voidResult?Void.class:Value.class)&&m.getGenericReturnType()==m.getReturnType()&&!m.isBridge()&&Modifier.isFinal(m.getModifiers()));typed++;}
        }
        require(typed==1 && erased==1);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean voidResult=Boolean.parseBoolean(args[2]);metadata(voidResult);
        RuntimeException failure=new IllegalStateException("owned raw zero failure");Trace.failure=failure;Trace.failAt=mode>=1&&mode<=4?mode:0;
        for(int attempt=0;attempt<2;attempt++) {
            Value first=mode==5?null:new Value(),replacement=mode==6?null:mode==7?first:new Value();Object sentinel=new Object();
            Trace.events=0;Trace.value=first;Trace.replacement=replacement;Trace.current=sentinel;
            Object result=null;Throwable caught=null;try{result=call(route);}catch(Throwable e){caught=e;}
            String sequence=attempt==0?"1234":mode==1?"":"234";
            if(Trace.failAt!=0&&sequence.indexOf((char)('0'+Trace.failAt))>=0)sequence=sequence.substring(0,sequence.indexOf((char)('0'+Trace.failAt))+1);
            require(Trace.events==(sequence.isEmpty()?0:Long.parseLong(sequence)));
            if(mode==1)require(attempt==0?caught instanceof ExceptionInInitializerError&&caught.getCause()==failure:caught instanceof NoClassDefFoundError);
            else if(mode>=2&&mode<=4)require(caught==failure);
            else require(caught==null && result==first);
            require(Trace.current==(mode==1||mode==2?sentinel:first));
            require(Trace.value==(mode==1||mode==2||mode==3?first:replacement));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
