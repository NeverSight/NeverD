import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ClosedDefaultAccessHarness {
    static void require(boolean value) { if(!value)throw new AssertionError("owned generic default access"); }
    static Vector call(int route,DefaultApi receiver,Link<String,Bound> link) throws Throwable {
        try {
            if(route==0)return DefaultApi.DefaultImpls.work(receiver,link);
            if(route==1)return ClosedDefaultAccessCaller.call(receiver,link);
            if(route==2)return (Vector)DefaultApi.class.getDeclaredMethod("access$work$jd",DefaultApi.class,Link.class)
                .invoke(null,receiver,link);
            return (Vector)MethodHandles.publicLookup().findStatic(DefaultApi.class,"access$work$jd",
                MethodType.methodType(Vector.class,DefaultApi.class,Link.class)).invokeExact(receiver,(Link)link);
        }catch(InvocationTargetException failure){throw failure.getCause();}
    }
    static void signature(Method method) {
        TypeVariable<Method>[] formals=method.getTypeParameters();
        require(formals.length==0);
        ParameterizedType input=(ParameterizedType)method.getGenericParameterTypes()[0];
        ParameterizedType result=(ParameterizedType)method.getGenericReturnType();
        require(input.getRawType()==Link.class && input.getActualTypeArguments()[0]==String.class && input.getActualTypeArguments()[1]==Bound.class);
        require(result.getRawType()==Vector.class && result.getActualTypeArguments()[0]==Bound.class);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),variant=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        boolean original=args[3].equals("work");
        Method accessor=DefaultApi.class.getDeclaredMethod("access$work$jd",DefaultApi.class,Link.class);
        require(accessor.isSynthetic()==original && Modifier.isStatic(accessor.getModifiers()) && accessor.getTypeParameters().length==0);
        Method entry=DefaultApi.class.getMethod("work",Link.class);require(entry.isDefault());signature(entry);
        if(!original){Method helper=DefaultApi.class.getDeclaredMethod(args[3],Link.class);require(Modifier.isPrivate(helper.getModifiers()) && !Modifier.isStatic(helper.getModifiers()));signature(helper);}
        String input=mode==1?null:new String("owned input");
        DefaultApi receiver=variant==0?new DefaultBase(input):new DefaultOverride(input);
        RuntimeException failure=new IllegalArgumentException("owned generic failure");
        int[] failures={0,0,0,0,1,2,3,4,5,6,7,0,0};
        Object previous=null;
        for(int attempt=0;attempt<2;attempt++){
            Bound value=mode==11?null:new Bound(0x1234567887654321L+attempt);
            Link<String,Bound> callback=new Link<String,Bound>(){ public Bound apply(String actual){require(actual==input);Trace.hit(3);return value;} };
            Trace.events=0;Trace.overrides=0;Trace.failAt=failures[mode];Trace.failure=failure;
            Trace.input=Trace.link=Trace.result=Trace.partial=null;
            Trace.wide=mode==12?0x7fedcba987654321L:-0x1234567887654321L;
            boolean failing=mode==2||mode==3||Trace.failAt!=0;
            try{
                Vector actual=call(route,mode==2?null:receiver,mode==3?null:callback);
                require(!failing && actual==Trace.result && actual==Trace.partial && actual!=previous && actual.owner==receiver && actual.value==value && actual.wide==Trace.wide);
                previous=actual;
            }catch(Throwable problem){require(failing && ((mode==2||mode==3)?problem instanceof NullPointerException:problem==failure));}
            long expected=mode==2?0:mode==3?12:mode==4?1:mode==5?12:mode==6?123:mode==7?1234:mode==8?12345:mode==9?123456:1234567;
            require(Trace.events==expected && Trace.overrides==0);
            require(Trace.link==((mode==2||mode==4||mode==3)?null:callback));
            require(Trace.input==((mode==2||mode==4||mode==5)?null:input));
            if(mode==8){Vector partial=(Vector)Trace.partial;require(partial!=null && partial.owner==null && partial.value==null && partial.wide==0 && Trace.result==null);}
            else if(mode==9){Vector partial=(Vector)Trace.partial;require(partial!=null && partial.owner==receiver && partial.value==value && partial.wide==Trace.wide && Trace.result==null);}
            else if(!failing||mode==10)require(Trace.result==Trace.partial && Trace.result!=null);
            else require(Trace.partial==null && Trace.result==null);
            Trace.events=0;Trace.failAt=0;Trace.overrides=0;
            Trace.overrideResult=new Vector<Bound>(receiver,value,Trace.wide);Trace.events=0;
            Vector virtual=receiver.work(callback);
            require(variant==0?virtual==Trace.result && Trace.events==1234567 && Trace.overrides==0:virtual==Trace.overrideResult && Trace.events==9 && Trace.overrides==1);
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",trace="+expected);
        }
    }
}
