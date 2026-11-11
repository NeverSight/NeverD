package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.Array;

public final class ArrayTypeTokenHarness {
    private static void check(boolean condition) {
        if (!condition) throw new AssertionError("owned array type token mismatch");
    }
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value) throws T {
        throw (T)value;
    }
    private static final class Observer extends Trace {
        final int route,mode,length;
        final Object source,token;
        final Throwable marker;
        final String later;
        final Object[] slots;
        int stage;
        Object[] expected,actual;
        Class<?> failedClass;
        boolean failed;
        Observer(int route,int mode,Object source,int length,Object token,Throwable marker,String later) {
            this.route=route;this.mode=mode;this.source=source;this.length=length;
            this.token=token;this.marker=marker;this.later=later;
            slots=source instanceof Object[] ? ((Object[])source).clone() : null;
        }
        void fail(int wanted) {
            if(mode==wanted) {failed=true;throw unchecked(marker);}
        }
        @Override public void observe(int phase,Object array,int count,Object type,Object value) {
            check(array==source && count==length && type==token);
            if(phase==3) {check(stage==0 && value==null);stage=1;fail(8);return;}
            if(phase==1) {
                check(stage==1 && value==null);stage=2;fail(9);
                if(mode==14) {((Object[])source)[0]=later;slots[0]=later;}
                try {
                    expected=(Object[])Array.newInstance(((Class<?>)token).getComponentType(),length);
                    System.arraycopy(source,0,expected,0,Math.min(((Object[])source).length,length));
                } catch(Throwable problem) {failedClass=problem.getClass();}
                return;
            }
            if(phase==2) {
                check(stage==2 && failedClass==null && value instanceof Object[]);
                actual=(Object[])value;check(actual!=source && actual.getClass()==expected.getClass());
                check(actual.length==expected.length);
                for(int i=0;i<expected.length;++i)check(actual[i]==expected[i]);
                stage=3;
                if(mode==16) {actual[0]=later;expected[0]=later;}
                fail(10);return;
            }
            check(phase==4 && stage==3 && (route==2 ? value==null : value==actual));
            stage=4;fail(11);
        }
        void finish(Object result,Throwable problem) {
            if(mode==4 || mode==5 || mode==6)check(problem instanceof ClassCastException && stage==0);
            else if(mode>=8 && mode<=11)check(failed && problem==marker && result==null);
            else if(failedClass!=null)check(problem!=null && problem.getClass()==failedClass && stage==2);
            else {
                check(problem==null && stage==4 && (route==2 ? result==null : result==actual));
                for(int i=0;i<expected.length;++i)check(actual[i]==expected[i]);
            }
            if(slots!=null) {
                Object[] array=(Object[])source;check(array.length==slots.length);
                for(int i=0;i<slots.length;++i)check(array[i]==slots[i]);
            }
        }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        check(route>=0 && route<3 && mode>=0 && mode<25);
        for(int attempt=0;attempt<2;++attempt) {
            String a=new String("a"),b=new String("b"),later=new String("later");
            Object source=new Object[]{a,b};Object token=Object[].class;int length=2;
            if(mode==1 || mode==2)source=new String[]{a,b};
            if(mode==2 || mode==13)token=String[].class;
            if(mode==3 || mode==15)source=null;
            if(mode==4)source=new Object();
            if(mode==5)source=new int[]{1,2};
            if(mode==6)token=new Object();
            if(mode==7 || mode==24)token=null;
            if(mode==12 || mode==24)length=-1;
            if(mode==13)source=new Object[]{a,new Object()};
            if(mode==15 || mode==21)length=0;
            if(mode==17)token=int[].class;
            if(mode==18)token=String.class;
            if(mode==19)length=5;
            if(mode==20)length=1;
            if(mode==21)token=Integer[].class;
            if(mode==22) {source=new Object[]{null,b};token=String[].class;}
            if(mode==23) {source=new Object[][]{{a},{b}};token=String[][].class;}
            Throwable marker=mode==10 || mode==11 ? new Fault() : new Failure();
            Observer observer=new Observer(route,mode,source,length,token,marker,later);
            Trace.hook=observer;Object result=null;Throwable problem=null;
            try {
                if(route==0)result=Core.run(source,length,token);
                else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                    MethodType.methodType(Object[].class,Object.class,int.class,Object.class)).invokeWithArguments(source,length,token);
                else Core.ignore(source,length,token);
            } catch(Throwable failure) {problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,array-type-token=matched");
    }
}
