import fixture.*;
import java.lang.reflect.*;
public class GenericTerminalVoidHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned generic terminal void");}
    static void signature(Method m,Class result) {
        TypeVariable[] vars=m.getTypeParameters();require(vars.length==1&&vars[0].getBounds().length==1&&vars[0].getBounds()[0]==Object.class);
        Type[] args=m.getGenericParameterTypes();require(args.length==2&&args[0] instanceof ParameterizedType&&args[1].equals(vars[0]));
        ParameterizedType key=(ParameterizedType)args[0];require(key.getRawType()==Key.class&&key.getActualTypeArguments().length==1&&key.getActualTypeArguments()[0].equals(vars[0])&&m.getGenericReturnType()==result);
    }
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),entry=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);String target=args[3];boolean projected=Integer.parseInt(args[4])!=0;
        Method typed=GenericTerminalVoidCaller.method(target,Void.class),bridge=GenericTerminalVoidCaller.method("writeValue",void.class);
        require(typed.getModifiers()==Modifier.PUBLIC&&bridge.isBridge()==!projected&&bridge.isSynthetic()==!projected);
        require(bridge.getTypeParameters().length==0&&bridge.getGenericParameterTypes()[0]==Key.class&&bridge.getGenericParameterTypes()[1]==Object.class);
        signature(typed,Void.class);signature(Base.class.getDeclaredMethod("writeValue",Key.class,Object.class),void.class);
        require(Core.class.getSuperclass()==Base.class&&Key.class.getTypeParameters().length==1&&Key.class.getTypeParameters()[0].getBounds()[0]==Object.class);
        Throwable previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            GenericTerminalVoidProbe.touches=0;Object key=mode%3==0?null:new Key();
            Object[] values={null,new Object(),Integer.valueOf(7),Long.valueOf(Long.MIN_VALUE),new StringBuilder("owned"),new IllegalStateException("owned value"),new AssertionError("owned value"),new byte[2],new Object[2],new Key(),Core.INSTANCE,Boolean.TRUE,Double.valueOf(Double.NaN),Key.class,key,attempt==0?new GenericTerminalVoidProbe():new GenericTerminalVoidProbe.Child()};
            Object value=values[mode];Throwable failure=null;
            try{GenericTerminalVoidCaller.call(route,entry,key,value,target);}catch(Throwable e){failure=e;}
            require(failure!=null&&failure.getClass()==IllegalStateException.class&&"owned terminal message".equals(failure.getMessage())&&failure.getCause()==null&&failure!=previous&&GenericTerminalVoidProbe.touches==0);
            StackTraceElement[] frames=failure.getStackTrace();int own=0;
            for(StackTraceElement frame:frames)if(frame.getClassName().equals("fixture.Core"))own++;
            require(frames.length>0&&frames[0].getClassName().equals("fixture.Core")&&frames[0].getMethodName().equals(target)&&own==(entry==0?2:1));
            if(entry==0)require(frames[1].getClassName().equals("fixture.Core")&&frames[1].getMethodName().equals("writeValue"));
            previous=failure;
            System.out.println("route="+route+",entry="+entry+",mode="+mode+",attempt="+attempt+",frames="+own);
        }
    }
}
