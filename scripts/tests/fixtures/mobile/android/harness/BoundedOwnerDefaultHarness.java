import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class BoundedOwnerDefaultHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned bounded owner default"); }
    static Vector call(int route,DefaultApi receiver,Vector a,Vector b,Vector c,String name)throws Throwable {
        if(route==0)return receiver.finish(a,b,c);
        if(route==1)return BoundedOwnerDefaultCaller.call(receiver,a,b,c);
        if(route==2) {
            if(name.equals("finish")) {
                try{return(Vector)DefaultApi.DefaultImpls.class.getMethod("finish",DefaultApi.class,Vector.class,Vector.class,Vector.class).invoke(null,receiver,a,b,c);}
                catch(InvocationTargetException f){throw f.getCause();}
            }
            Method helper=DefaultApi.class.getDeclaredMethod(name,Vector.class,Vector.class,Vector.class);
            helper.setAccessible(true);
            try{return(Vector)helper.invoke(receiver,a,b,c);}catch(InvocationTargetException f){throw f.getCause();}
        }
        MethodHandle handle=MethodHandles.publicLookup().findStatic(DefaultApi.DefaultImpls.class,"finish",MethodType.methodType(Vector.class,DefaultApi.class,Vector.class,Vector.class,Vector.class));
        return(Vector)handle.invokeExact(receiver,a,b,c);
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),variant=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);String name=args[3];
        TypeVariable<?>[] formals=DefaultApi.class.getTypeParameters();require(formals.length==1 && formals[0].getBounds()[0]==Vector.class);
        Method method=DefaultApi.class.getMethod("finish",Vector.class,Vector.class,Vector.class);
        for(Type p:method.getGenericParameterTypes())require(p.equals(formals[0]));require(method.getGenericReturnType().equals(formals[0]));
        long[] wide={0L,Long.MIN_VALUE,Long.MAX_VALUE,0x12345678abcdef01L,-0x12345678abcdef0L};
        for(int attempt=0;attempt<2;attempt++) {
            Vector a=mode==1?null:new Vector(11),b=mode==2?null:new Vector(22),c=mode==3?null:new Vector(33);
            DefaultApi receiver=mode==4?null:variant==0?new DefaultBase<Vector>():new DefaultOverride<Vector>();
            Trace.events=Trace.received=0;Trace.wide=wide[(mode+attempt)%wide.length];Trace.overrides=0;Trace.first=Trace.second=Trace.third=null;
            Trace.failAt=mode>=5&&mode<=9?mode-4:0;Trace.failure=new RuntimeException("owned bounded failure");
            Vector actual=null;Throwable caught=null;try{actual=call(route,receiver,a,b,c,name);}catch(Throwable f){caught=f;}
            boolean virtual=mode!=4&&route==0&&variant==1;
            long expected=mode==4?0:virtual?9:Trace.failAt==1?1:Trace.failAt==2?12:Trace.failAt==3?123:Trace.failAt==4?1234:12345;
            if(mode==4)require(caught instanceof NullPointerException);
            else if(!virtual&&Trace.failAt!=0)require(caught==Trace.failure);
            else require(caught==null && actual==(virtual?b:c));
            require(Trace.events==expected && Trace.overrides==(virtual?1:0));
            if(mode!=4&&!virtual) {
                require(Trace.first==a&&Trace.second==b&&Trace.third==c);
                if(Trace.failAt==0||Trace.failAt>=4)require(Trace.received==Trace.wide);
                else require(Trace.received==0);
            }
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
