import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.Arrays;
import kotlin.Unit;
import kotlin.jvm.functions.Function2;
public final class OwnerVoidDefaultHarness {
    static void require(boolean value) { if(!value)throw new AssertionError("owned scoped void default"); }
    static void call(int route,DefaultApi receiver,Function2<Object,Object,Unit> block,Object value) throws Throwable {
        try {
            if(route==0)DefaultApi.DefaultImpls.work(receiver,block,value);
            else if(route==1)OwnerVoidDefaultCaller.call(receiver,block,value);
            else if(route==2)DefaultApi.class.getDeclaredMethod("access$work$jd",DefaultApi.class,Function2.class,Object.class).invoke(null,receiver,block,value);
            else MethodHandles.publicLookup().findStatic(DefaultApi.class,"access$work$jd",
                MethodType.methodType(void.class,DefaultApi.class,Function2.class,Object.class)).invokeExact(receiver,(Function2)block,value);
        } catch(InvocationTargetException problem) { throw problem.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),variant=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        boolean original=args[3].equals("work");
        Method entry=DefaultApi.class.getDeclaredMethod("work",Function2.class,Object.class);
        require(entry.isDefault()&&entry.getReturnType()==void.class&&entry.getTypeParameters().length==0);
        TypeVariable<?> formal=DefaultApi.class.getTypeParameters()[0];
        require(formal.getName().equals("N")&&formal.getBounds()[0]==Object.class);
        ParameterizedType callback=(ParameterizedType)entry.getGenericParameterTypes()[0];
        require(callback.getRawType()==Function2.class);
        WildcardType input=(WildcardType)callback.getActualTypeArguments()[0];
        require(input.getLowerBounds().length==1&&input.getLowerBounds()[0].equals(formal));
        require(callback.getActualTypeArguments()[1]==Object.class&&callback.getActualTypeArguments()[2]==Unit.class);
        Method accessor=DefaultApi.class.getDeclaredMethod("access$work$jd",DefaultApi.class,Function2.class,Object.class);
        require(accessor.isSynthetic()==original&&Modifier.isStatic(accessor.getModifiers())&&accessor.getTypeParameters().length==0);
        if(!original){
            Method helper=DefaultApi.class.getDeclaredMethod(args[3],Function2.class,Object.class);
            require(Modifier.isPrivate(helper.getModifiers())&&!Modifier.isStatic(helper.getModifiers()));
            require(Arrays.equals(helper.getGenericParameterTypes(),entry.getGenericParameterTypes()));
        }
        for(int attempt=0;attempt<2;attempt++) {
            Object snapshot=mode==1?null:new Object(),value=mode==2?null:new Object();
            DefaultBase receiver=variant==0?new DefaultBase(snapshot):new DefaultOverride(snapshot);
            RuntimeException failure=new IllegalArgumentException("owned scoped void failure");
            int[] failures={0,0,0,0,0,1,2,3,4,5,0,0};
            Trace.events=0;Trace.overrides=0;Trace.calls=0;Trace.failAt=failures[mode];Trace.failure=failure;
            Trace.input=Trace.value=Trace.receiver=null;
            Function2<Object,Object,Unit> block=new Function2<Object,Object,Unit>() {
                public Unit invoke(Object actual,Object other) {
                    require(actual==snapshot&&other==value&&Trace.input==snapshot&&Trace.receiver==receiver);
                    Trace.calls++;Trace.value=other;Trace.hit(4);
                    if(mode==11)receiver.currentValue=new Object();
                    return mode==10?null:Unit.INSTANCE;
                }
            };
            boolean failing=mode==3||mode==4||Trace.failAt!=0;
            try { call(route,mode==3?null:receiver,mode==4?null:block,value);require(!failing); }
            catch(Throwable problem){require(failing&&((mode==3||mode==4)?problem.getClass()==NullPointerException.class:problem==failure));}
            long expected=mode==3?0:mode==4?123:mode==5?1:mode==6?12:mode==7?123:mode==8?1234:12345;
            require(Trace.events==expected&&Trace.overrides==0);
            require(Trace.receiver==((mode==3||mode==5)?null:receiver));
            require(Trace.input==((mode==3||mode==5||mode==6)?null:snapshot));
            require(Trace.calls==((mode>=3&&mode<=7)?0:1));
            require(Trace.value==((mode>=3&&mode<=7)?null:value));
            if(mode==11)require(receiver.currentValue!=snapshot&&Trace.input==snapshot);
            Trace.events=0;Trace.failAt=0;Trace.calls=0;Trace.overrides=0;Trace.input=Trace.value=Trace.receiver=null;
            receiver.currentValue=snapshot;receiver.work(block,value);
            require(variant==0?Trace.events==12345&&Trace.overrides==0&&Trace.calls==1:Trace.events==9&&Trace.overrides==1&&Trace.calls==0);
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
