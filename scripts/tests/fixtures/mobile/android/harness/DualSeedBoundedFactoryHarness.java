import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class DualSeedBoundedFactoryHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned dual seeded bounded factory"); }
    static final class Convert implements Converter<Object,Bound> {
        final Bound result; Object observed;
        Convert(Bound result) { this.result=result; }
        public Bound convert(Object value) { observed=value; Trace.hit(3); return result; }
    }
    static final class Observe implements Callback<Object,Unit> {
        final Unit result; Object observed;
        Observe(Unit result) { this.result=result; }
        public Unit apply(Object value) { observed=value; Trace.hit(5); return result; }
    }
    static final class WrongConvert implements Converter<String,Bound> {
        public Bound convert(String value) { Trace.hit(3); return new Bound(); }
    }
    static final class WrongCallback implements Callback<String,Unit> {
        public Unit apply(String value) { Trace.hit(5); return new Unit(); }
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    static Carrier call(int route,Object first,Converter converter,Carrier input,Object second,
            String label,Callback callback,int kind) throws Throwable {
        try {
            if(route==0)return Client.call(first,converter,input,second,label,callback,kind);
            if(route==1)return DualSeedBoundedFactoryCaller.call(first,converter,input,second,label,callback,kind);
            if(route==2)return (Carrier)Provider.class.getMethod("create",Object.class,Converter.class,
                Carrier.class,Object.class,String.class,Callback.class,int.class)
                .invoke(null,first,converter,input,second,label,callback,kind);
            return (Carrier)MethodHandles.publicLookup().findStatic(Provider.class,"create",
                MethodType.methodType(Carrier.class,Object.class,Converter.class,Carrier.class,Object.class,
                String.class,Callback.class,int.class)).invokeExact(first,converter,input,second,label,callback,kind);
        } catch(InvocationTargetException failure) { throw failure.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Method factory=Provider.class.getMethod("create",Object.class,Converter.class,Carrier.class,Object.class,
            String.class,Callback.class,int.class);
        TypeVariable<?>[] formals=factory.getTypeParameters(); Type[] params=factory.getGenericParameterTypes();
        require(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Bound.class);
        require(params[0].equals(formals[0]) && params[3].equals(formals[0]));
        Type[] pair=((ParameterizedType)params[1]).getActualTypeArguments();
        require(pair[0].equals(formals[0]) && pair[1].equals(formals[1]));
        require(((ParameterizedType)params[2]).getActualTypeArguments()[0].equals(formals[0]));
        Type[] cb=((ParameterizedType)params[5]).getActualTypeArguments();
        require(((WildcardType)cb[0]).getLowerBounds()[0].equals(formals[0]) && cb[1]==Unit.class);
        require(((ParameterizedType)factory.getGenericReturnType()).getActualTypeArguments()[0].equals(formals[0]));
        int[] failures={0,0,0,0,1,2,3,4,5,6,7,0,0,0,0,0,0,0,0};
        RuntimeException original=new IllegalArgumentException("owned factory failure");
        Carrier previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            Object first=mode==1?null:new Object(),second=mode==2?null:new Object();
            Bound middle=mode==17?null:new Bound(); Unit unit=mode==17?null:new Unit();
            Convert converter=new Convert(middle); Observe callback=new Observe(unit);
            Converter selected=mode==11?null:mode==18?new WrongConvert():converter;
            Callback selectedCallback=mode==12?null:mode==13?new WrongCallback():callback;
            Carrier input=mode==3?null:new Carrier<Object>(new Object(),new Object());
            Carrier supplied=new Carrier<Object>(new Object(),new Object());
            String label=mode==3?null:"owned label";
            int kind=mode==14?1:mode==15?2:mode==16?3:0;
            Trace.events=0;Trace.first=Trace.second=Trace.converter=Trace.input=Trace.callback=null;
            Trace.converted=Trace.callbackResult=null;Trace.label=null;Trace.supplied=supplied;
            Trace.failAt=failures[mode];Trace.failure=original;
            boolean failing=Trace.failAt!=0 || mode==11 || mode==12 || mode==13 || mode==18;
            Carrier actual=null;
            try {
                actual=call(route,first,selected,input,second,label,selectedCallback,kind);
                require(!failing);
            } catch(Throwable problem) {
                require(failing && (Trace.failAt!=0?problem==original:
                    mode==13||mode==18?problem instanceof ClassCastException:problem instanceof NullPointerException));
            }
            long expected=mode==4?1:mode==5||mode==11||mode==18?12:mode==6?123:
                mode==7||mode==12||mode==13?1234:mode==8?12345:
                mode==9||kind!=0?123456:1234567;
            require(Trace.events==expected);
            if(mode!=4)require(Trace.first==first && Trace.second==second && Trace.converter==selected &&
                Trace.input==input && Trace.callback==selectedCallback && Trace.label==label);
            require(converter.observed==(mode==4||mode==5||mode==11||mode==18?null:first));
            require(Trace.converted==(mode>=4&&mode<=6||mode==11||mode==18?null:middle));
            require(callback.observed==(mode>=4&&mode<=7||mode>=11&&mode<=13||mode==18?null:second));
            require(Trace.callbackResult==(mode>=4&&mode<=8||mode>=11&&mode<=13||mode==18?null:unit));
            if(!failing) {
                if(kind==1)require(actual==supplied);
                else if(kind==2)require(actual==null);
                else if(kind==3)require(actual==input);
                else require(actual!=previous && actual.first()==first && actual.second()==second);
            }
            previous=actual;
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
