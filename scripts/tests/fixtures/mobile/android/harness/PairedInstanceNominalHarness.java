package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.function.BiFunction;

public final class PairedInstanceNominalHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned paired instance nominal mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {throw (T)value;}
    private static final class Source extends Provider {
        Observer observer;
        @Override public <A,B> Object bind(Class<A> key,Class<B> token,BiFunction<? super A,? super B,Boolean> predicate) {
            Observer o=observer;check(o.overrides++==0);
            Object result=super.bind(key,token,predicate);
            if(o.mode==21){Trace.returned=o.later;return o.overrideResult;}
            return result;
        }
        @Override public <A,B> void consume(Class<A> first,Class<B> second,BiFunction<? super A,? super B,Boolean> callback) {
            Observer o=observer;check(o.overrides++==0);
            super.consume(first,second,callback);
            if(o.mode==21)Trace.returned=o.later;
        }
        @Override public <A,B> long measure(Class<A> first,Class<B> second,BiFunction<? super A,? super B,Boolean> callback) {
            Observer o=observer;check(o.overrides++==0);
            long result=super.measure(first,second,callback);
            if(o.mode==21){Trace.returned=o.later;Trace.number=o.laterNumber;return o.overrideNumber;}
            return result;
        }
    }
    private static final class Sink implements BiFunction<String,Integer,Boolean> {
        Observer observer;
        @Override public Boolean apply(String value,Integer second) {
            Observer o=observer;check(o.stage==2 && o.calls++==0 && value==o.expectedValue && second==o.expectedSecond);
            o.fail(9);
            if(o.mode==14){Trace.returned=o.later;Trace.number=o.laterNumber;}
            if(o.mode==20){Trace.firstValue=o.replacement;o.failed=true;throw unchecked(o.marker);}
            if(o.mode==29){Trace.secondValue=null;Trace.returned=o.later;Trace.number=o.laterNumber;}
            return o.wanted;
        }
    }
    private static final class Observer extends Trace {
        final int route,mode;
        final Object owner,key,token,predicate;
        final Throwable marker;
        final String replacement;
        final Object later,overrideResult;
        final Boolean wanted;
        int stage,calls,overrides;
        long numberResult;
        final long laterNumber=0x71234567abcdef01L,overrideNumber=0x8123456789abcdefL;
        boolean failed;
        Object expectedValue,expectedSecond;
        Object result;
        Observer(int route,int mode,Object owner,Object key,Object token,Object predicate,
                 Throwable marker,String replacement,Provider later,Provider overrideResult,Object value,Object second) {
            this.route=route;this.mode=mode;this.owner=owner;this.key=key;this.token=token;
            this.predicate=predicate;this.marker=marker;this.replacement=replacement;
            this.later=later;this.overrideResult=overrideResult;expectedValue=value;expectedSecond=second;wanted=mode==28?null:Boolean.valueOf(mode!=1);
        }
        void fail(int wantedMode){if(mode==wantedMode){failed=true;throw unchecked(marker);}}
        @Override public void observe(int phase,Provider self,Class name,Class type,Object callback,Object value) {
            check(self==owner && name==key && type==token && callback==predicate);
            if(phase==3){check(stage==0 && value==null);stage=1;fail(8);return;}
            if(phase==1){
                check(stage==1 && value==null && overrides==1);stage=2;
                if(mode==15){Trace.firstValue=replacement;expectedValue=replacement;}
                return;
            }
            if(phase==2){
                check(stage==2 && calls==1 && value==Trace.returned && Trace.accepted==wanted);
                result=value;numberResult=Trace.number;stage=3;
                if(mode==14 || mode==29)check(result==later);
                if(mode==16){Trace.returned=later;Trace.number=laterNumber;}
                fail(10);return;
            }
            Object expected=mode==21 ? overrideResult : result;
            check(phase==4 && stage==3 && (route==2 || route>=4 ? value==null : value==expected));
            stage=4;if(mode==17){Trace.returned=later;Trace.number=laterNumber;}fail(11);
        }
        void finish(Object returned,Throwable problem) {
            if(mode>=4 && mode<=7)check(problem instanceof ClassCastException && stage==0 && overrides==0);
            else if(mode==3)check(problem instanceof NullPointerException && stage==1 && overrides==0);
            else if(mode==12 || mode==24 || mode==26)check(problem instanceof ClassCastException && stage==2 && calls==0);
            else if(mode==13)check(problem instanceof NullPointerException && stage==2 && calls==0);
            else if((mode>=8 && mode<=11) || mode==20)check(failed && problem==marker && returned==null);
            else {
                Object expected=mode==21 ? overrideResult : result;
                check(problem==null && stage==4);
                if(route==5)check(returned instanceof Long && ((Long)returned).longValue()==(mode==21?overrideNumber:numberResult));
                else check(route==2 || route==4 ? returned==null : returned==expected);
            }
            if(stage>=2)check(overrides==1);
            if(stage>=3)check(calls==1 && Trace.accepted==wanted);
            if(mode==14 || mode==16 || mode==17 || mode==21 || mode==29)check(Trace.returned==later);
            if(mode==15 || mode==20)check(Trace.firstValue==replacement);
            if(mode==29)check(Trace.secondValue==null);
        }
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        java.lang.reflect.Method declaration=Provider.class.getMethod("bind",Class.class,Class.class,BiFunction.class);
        java.lang.reflect.TypeVariable<?>[] formals=declaration.getTypeParameters();
        check(formals.length==2 && formals[0].getBounds()[0]==Object.class && formals[1].getBounds()[0]==Object.class && declaration.getGenericReturnType()==Object.class);
        java.lang.reflect.Type[] parameters=declaration.getGenericParameterTypes();
        for(int i=0;i<2;i++)check(((java.lang.reflect.ParameterizedType)parameters[i]).getActualTypeArguments()[0].equals(formals[i]));
        java.lang.reflect.Type[] slots=((java.lang.reflect.ParameterizedType)parameters[2]).getActualTypeArguments();
        for(int i=0;i<2;i++)check(((java.lang.reflect.WildcardType)slots[i]).getLowerBounds()[0].equals(formals[i]));
        check(slots[2]==Boolean.class);
        for(String name:new String[]{"consume","measure"}) {
            java.lang.reflect.Method m=Provider.class.getMethod(name,Class.class,Class.class,BiFunction.class);
            check(m.getTypeParameters().length==2 && m.getGenericReturnType()==(name.equals("consume")?void.class:long.class));
            java.lang.reflect.Type[] types=m.getGenericParameterTypes();
            for(int i=0;i<2;i++)check(((java.lang.reflect.ParameterizedType)types[i]).getActualTypeArguments()[0].equals(m.getTypeParameters()[i]));
        }
        check(route>=0 && route<6 && mode>=0 && mode<30);
        for(int attempt=0;attempt<2;++attempt){
            Source source=new Source();Sink sink=new Sink();
            String value=new String("value"),replacement=new String("replacement");
            Provider ordinary=new Provider(),later=new Provider(),overrideResult=new Provider();
            Object owner=source,name=String.class,token=Integer.class,callback=sink;
            if(mode==3)owner=null;
            if(mode==4)owner=new Object();
            if(mode==5)name=new Object();
            if(mode==6)token=new Object();
            if(mode==7)callback=new Object();
            if(mode==13)callback=null;
            if(mode==18){name=Integer.class;token=String.class;}
            if(mode==23)token=null;
            if(mode==25)name=null;
            Trace.firstValue=mode==12 ? Integer.valueOf(7) : mode==19 ? null : mode==24 ? new String[]{value} : value;
            Trace.secondValue=mode==26 ? new Object() : mode==27 ? null : Integer.valueOf(29);
            Trace.returned=mode==2 ? null : mode==22 ? source : ordinary;
            Trace.accepted=Boolean.valueOf(mode==1);
            Trace.number=0x12345678fedcba98L;
            Throwable marker=mode==10 || mode==11 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,owner,name,token,callback,marker,replacement,later,overrideResult,Trace.firstValue,Trace.secondValue);
            source.observer=observer;sink.observer=observer;Trace.hook=observer;
            Object result=null;Throwable problem=null;
            try{
                if(route==0)result=Core.run(owner,name,token,callback);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(Object.class,Object.class,Object.class,Object.class,Object.class)).invokeWithArguments(owner,name,token,callback);
                else if(route==2)Core.ignore(owner,name,token,callback);
                else if(route==3){try{result=Core.class.getMethod("run",Object.class,Object.class,Object.class,Object.class).invoke(null,owner,name,token,callback);}catch(java.lang.reflect.InvocationTargetException failure){throw failure.getCause();}}
                else if(route==4)Core.consume(owner,name,token,callback);
                else result=Long.valueOf(Core.measure(owner,name,token,callback));
            }catch(Throwable failure){problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,paired-instance-nominal=matched");
    }
}
