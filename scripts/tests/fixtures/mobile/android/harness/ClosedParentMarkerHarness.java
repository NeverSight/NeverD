import api.Function;
import fixture.*;
import kotlin.jvm.internal.DefaultConstructorMarker;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class ClosedParentMarkerHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned closed parent marker"); }
    static Object field(Object value,String name) throws Throwable { Field f=value.getClass().getDeclaredField(name);f.setAccessible(true);return f.get(value); }
    static Object call(int route,Constructor<?> constructor,long bits,Context context,float weight,Token token,Function callback,DefaultConstructorMarker marker) throws Throwable {
        if(route==0)return Facade.create(bits,context,weight,token,callback,marker);
        if(route==1)return ClosedParentMarkerCaller.call(bits,context,weight,token,callback,marker);
        if(route==2)try { return constructor.newInstance(bits,context,weight,token,callback,marker); }catch(InvocationTargetException error) { throw error.getCause(); }
        return MethodHandles.lookup().unreflectConstructor(constructor).invokeWithArguments(bits,context,weight,token,callback,marker);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean synthetic=Boolean.parseBoolean(args[2]);
        Class<?> core=Class.forName("fixture.Core"),local=Class.forName("fixture.LocalToken");
        require(Modifier.isFinal(core.getModifiers())&&!Modifier.isPublic(core.getModifiers())&&core.getTypeParameters().length==0&&!Modifier.isPublic(local.getModifiers()));
        ParameterizedType parent=(ParameterizedType)core.getGenericSuperclass();require(parent.getRawType()==Parent.class&&parent.getActualTypeArguments()[0]==local&&local.getSuperclass()==Token.class);
        Constructor<?> constructor=core.getDeclaredConstructor(long.class,Context.class,float.class,Token.class,Function.class,DefaultConstructorMarker.class);constructor.setAccessible(true);
        require(Modifier.isPublic(constructor.getModifiers())&&constructor.isSynthetic()==synthetic&&constructor.getGenericParameterTypes()[4]==Function.class);
        Constructor<?> primary=core.getDeclaredConstructor(long.class,Context.class,float.class,Token.class,Function.class);
        require(Modifier.isPrivate(primary.getModifiers())&&primary.getTypeParameters().length==0);
        Type[] cb=((ParameterizedType)primary.getGenericParameterTypes()[4]).getActualTypeArguments();require(((WildcardType)cb[0]).getLowerBounds()[0]==Context.class&&cb[1]==Unit.class);
        int[] lengths={6,6,6,6,6,6,3,1,2,3,4,5,6,6,4,6};int[] stages={7,1,2,8,3,4};
        for(int attempt=0;attempt<2;attempt++) {
            long bits=mode==1?Long.MIN_VALUE:mode==2?Long.MAX_VALUE:0x8123456789abcdefL+attempt;
            float weight=Float.intBitsToFloat(mode==1?0x80000000:mode==2?0x7f800000:mode==3?0x7fa12345:0x3f123456+attempt);
            Context context=mode==4?null:new Context();Token token=mode==5?null:new Token();Unit unit=mode==13?null:new Unit();
            Function callback=mode==6?null:new Function() { public Object invoke(Object value) { require(value==context&&Trace.core==Trace.parent);Trace.callbackInput=value;Trace.hit(8);return mode==14?"wrong unit":unit; } };
            DefaultConstructorMarker marker=mode==15?new DefaultConstructorMarker():null;
            Trace.events=Trace.parentCount=0;Trace.parent=Trace.core=Trace.callbackInput=null;Trace.unit=null;Trace.done=false;Trace.failure=new RuntimeException("owned failure");
            Trace.failAt=mode==7?7:mode==8?1:mode==9?2:mode==10?8:mode==11?3:mode==12?4:0;
            Object result=null;Throwable caught=null;
            try { result=call(route,constructor,bits,context,weight,token,callback,marker); }catch(Throwable error) { caught=error; }
            if(mode==6)require(caught instanceof NullPointerException);else if(mode==14)require(caught instanceof ClassCastException);else if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null&&result==Trace.core);
            int length=lengths[mode],events=0;for(int i=0;i<length;i++)events=events*10+stages[i];
            require(Trace.events==events&&Trace.parentCount==1&&Trace.parent!=null&&Trace.parent.getClass()==core);
            require(Trace.core==(length>=2?Trace.parent:null)&&Trace.callbackInput==(length>=4?context:null)&&Trace.unit==(length>=5?unit:null)&&Trace.done==(length==6));
            Object object=Trace.parent;boolean initialized=length>=3;
            require(((Long)field(object,"bits")).longValue()==(initialized?bits:0)&&field(object,"context")== (initialized?context:null)&&field(object,"token")== (initialized?token:null)&&field(object,"callback")== (initialized?callback:null));
            require(Float.floatToRawIntBits(((Float)field(object,"weight")).floatValue())==(initialized?Float.floatToRawIntBits(weight):0));
            for(Field f:core.getDeclaredFields())if(!Modifier.isStatic(f.getModifiers()))require(Modifier.isFinal(f.getModifiers()));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
