import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InterfaceDefaultAccessHarness {
    static void require(boolean ok) { if (!ok) throw new AssertionError("owned interface default access"); }
    static Object call(int route, DefaultApi receiver, Object value, String tag) throws Throwable {
        try {
            if (route == 0) return DefaultApi.DefaultImpls.work(receiver,value,tag);
            if (route == 1) return InterfaceDefaultAccessCaller.call(receiver,value,tag);
            if (route == 2) return DefaultApi.class.getDeclaredMethod("access$work$jd",DefaultApi.class,Object.class,String.class)
                .invoke(null,receiver,value,tag);
            return (Object)MethodHandles.publicLookup().findStatic(DefaultApi.class,"access$work$jd",
                MethodType.methodType(Object.class,DefaultApi.class,Object.class,String.class))
                .invokeExact(receiver,value,tag);
        } catch (InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]), variant=Integer.parseInt(args[1]), mode=Integer.parseInt(args[2]);
        String enclosing=args[3]; boolean input=enclosing.equals("work");
        Method accessor=DefaultApi.class.getDeclaredMethod("access$work$jd",DefaultApi.class,Object.class,String.class);
        require(accessor.isSynthetic()==input && Modifier.isStatic(accessor.getModifiers()));
        Method defaultEntry=DefaultApi.class.getMethod("work",Object.class,String.class);
        require(defaultEntry.isDefault());
        Object current=new Object();
        DefaultApi receiver=variant==0?new DefaultBase(current):new DefaultOverride(current);
        RuntimeException original=new IllegalArgumentException("owned default failure");
        int[] failures={0,0,0,1,2,6,3,5,4,0};
        Object previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Object value=mode==1?null:new Object();String tag=mode==9?null:"owned tag";
            Trace.events=0;Trace.overrides=0;Trace.failAt=failures[mode];Trace.failure=original;
            Trace.captured=Trace.current=Trace.returned=null;Trace.tag=null;
            boolean failing=mode==2||Trace.failAt!=0;
            try {
                Object actual=call(route,mode==2?null:receiver,value,tag);
                require(!failing && actual==value);
            } catch(Throwable failure) {
                require(failing && (mode==2?failure instanceof NullPointerException:failure==original));
            }
            long expected=mode==2?0:mode==3?1:mode==4?12:mode==5?126:mode==6?1263:mode==7?12635:126354;
            require(Trace.events==expected && Trace.overrides==0);
            if(mode!=2&&mode!=3)require(Trace.tag==tag);
            if(mode!=2&&mode!=3) {
                require(Trace.captured instanceof Callback && Trace.captured!=previous);
                Class<?> local=Trace.captured.getClass();Method owner=local.getEnclosingMethod();
                require(owner!=null && owner.getDeclaringClass()==DefaultApi.class && owner.getName().equals(enclosing));
                require(Modifier.isPrivate(owner.getModifiers())==!input);
                Field captured=local.getDeclaredField("$value");captured.setAccessible(true);
                require(captured.get(Trace.captured)==value);
                previous=Trace.captured;
            } else require(Trace.captured==null);
            if(mode!=2&&mode!=3&&mode!=4&&mode!=5)require(Trace.current==current);
            if(!failing||mode==8)require(Trace.returned==value);
            Trace.events=0;Trace.failAt=0;Trace.captured=null;Trace.overrides=0;
            Trace.overrideResult=new Object();
            Object virtual=receiver.work(value,tag);
            require(variant==0?virtual==value&&Trace.events==126354&&Trace.overrides==0
                :virtual==Trace.overrideResult&&Trace.events==9&&Trace.overrides==1);
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",defaultTrace="+expected);
        }
    }
}
