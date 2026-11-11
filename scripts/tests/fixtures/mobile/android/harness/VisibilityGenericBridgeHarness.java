package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import outside.Caller;
import outside.Sub;

public final class VisibilityGenericBridgeHarness {
    private static void check(boolean condition){if(!condition)throw new AssertionError("owned visibility generic bridge mismatch");}
    private static final class Failure extends Exception {}
    private static final class Fault extends Error {}
    @SuppressWarnings("unchecked")
    private static <T extends Throwable> RuntimeException unchecked(Throwable value)throws T{throw(T)value;}
    private static final class Observer extends Trace {
        final int route,mode,index;
        final Object self,first,second,values;
        final ArrayList<Object> list;
        final Object[] slots;
        final String[] array,other;
        final String replacement;
        final Throwable marker;
        final Object alternate;
        final boolean prefix;
        int state;
        Object expected,result;
        Observer(int route,int mode,int index,Object self,Object first,Object second,Object values,
                 ArrayList<Object> list,String[] array,String[] other,String replacement,Throwable marker,Object alternate) {
            this.route=route;this.mode=mode;this.index=index;this.self=self;this.first=first;this.second=second;
            this.values=values;this.list=list;this.array=array;this.other=other;this.replacement=replacement;
            this.marker=marker;this.alternate=alternate;prefix=route==0 || route==4 || route==5;
            slots=list.toArray();expected=array==null ? null : array[mode==1 ? 1 : 0];
        }
        void fail(int wanted){if(mode==wanted)throw unchecked(marker);}
        @Override public void observe(int phase,Object receiver,Object a,Object b,Object input,int flag,Object value) {
            check(receiver==self && a==first && b==second && input==values && flag==index);
            if(phase==4){check(prefix && state==0 && value==null);state=4;fail(18);return;}
            if(phase==1){check(route!=4 && state==(prefix ? 4 : 0) && value==null);state=1;fail(11);return;}
            if(phase==2){
                check(state==1 && value==array);state=2;
                if(mode==14){array[0]=replacement;expected=replacement;}
                if(mode==15){list.set(index,other);slots[index]=other;}
                fail(12);return;
            }
            if(phase==3){check(state==2 && value==expected);state=3;result=value;if(mode==16)array[0]=replacement;fail(13);return;}
            if(phase==5){check(route==4 && state==4 && value==null);state=5;if(mode==21)Trace.overrideReply=alternate;result=Trace.overrideReply;fail(20);return;}
            check(phase==6 && prefix && state==(route==4 ? 5 : 3) && (route==5 ? value==null : value==result));
            state=6;fail(19);
        }
        void finish(Object returned,Throwable problem){
            if(mode==3 || mode==4)check(problem instanceof ClassCastException && state==0);
            else if(mode==22)check(problem instanceof NullPointerException && state==(prefix ? 4 : 0));
            else if(mode==18 && prefix)check(problem==marker && state==4);
            else if(mode==19 && prefix)check(problem==marker && state==6);
            else if(route==4 && mode==20)check(problem==marker && state==5);
            else if(route!=4 && (mode==11 || mode==12 || mode==13))check(problem==marker && state==mode-10);
            else if(route!=4 && mode==2)check(problem instanceof NullPointerException && state==1);
            else if(route!=4 && mode==5)check(problem instanceof ClassCastException && state==1);
            else if(route!=4 && (mode==6 || mode==7))check(problem instanceof IndexOutOfBoundsException && state==1);
            else if(route!=4 && mode==8)check(problem instanceof NullPointerException && state==2);
            else if(route!=4 && (mode==9 || mode==10))check(problem instanceof ArrayIndexOutOfBoundsException && state==2);
            else check(problem==null && state==(prefix ? 6 : 3) && (route==5 ? returned==null : returned==result));
            check(list.size()==slots.length);
            for(int i=0;i<slots.length;++i)check(list.get(i)==slots[i]);
            if(route!=4 && (mode==14 || mode==16))check(array[0]==replacement);
        }
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);check(route>=0 && route<6 && mode>=0 && mode<25);
        Method reflected=Core.class.getDeclaredMethod("read",Object.class,Object.class,List.class,int.class);
        check(reflected.isBridge() && reflected.isSynthetic() && reflected.getModifiers()==4161);
        check(reflected.getGenericParameterTypes()[2]==List.class && reflected.getGenericReturnType()==Object.class);
        for(int attempt=0;attempt<2;++attempt){
            Object first=mode==23 ? null : new Object(),second=mode==24 ? null : new Object();
            String a=new String("a"),b=new String("b"),c=new String("c"),d=new String("d"),replacement=new String("replacement");
            String[] zero=new String[]{a,b},one=new String[]{c,d},other=new String[]{new String("other")};
            ArrayList<Object> list=new ArrayList<Object>();list.add(zero);list.add(one);
            int index=mode==1 ? 1 : mode==7 ? -1 : 0;Trace.offset=mode==1 ? 1 : mode==9 ? -1 : mode==10 ? 2 : 0;
            if(mode==5)list.set(0,new Object[]{new Object()});
            if(mode==6)list.clear();
            if(mode==8)list.set(0,null);
            if(mode==21)zero[0]=null;
            Object values=mode==2 ? null : mode==3 ? new Object() : list;
            Object receiver=route==3 || route==4 ? new Sub() : new Core();
            if(mode==4)receiver=new Object();if(mode==22)receiver=null;
            String[] selected=mode==1 ? one : mode==8 ? null : zero;
            Throwable marker=mode==12 || mode==19 ? new Fault() : new Failure();
            Object alternate=new Object();Trace.overrideReply=new Object();
            Observer observer=new Observer(route,mode,index,receiver,first,second,values,list,selected,other,replacement,marker,alternate);
            Trace.hook=observer;Object result=null;Throwable problem=null;
            try{
                if(route==0 || route==4)result=Caller.run(receiver,first,second,values,index);
                else if(route==5)Caller.ignore(receiver,first,second,values,index);
                else {
                    Core actual=(Core)receiver;List input=(List)values;
                    if(route==1)result=MethodHandles.lookup().findVirtual(Core.class,"read",MethodType.methodType(Object.class,Object.class,Object.class,List.class,int.class)).invokeWithArguments(actual,first,second,input,index);
                    else if(route==2){try{result=reflected.invoke(actual,first,second,input,Integer.valueOf(index));}catch(InvocationTargetException wrapped){throw wrapped.getCause();}}
                    else result=((Sub)actual).parent(first,second,input,index);
                }
            }catch(Throwable failure){problem=failure;}
            observer.finish(result,problem);check(Trace.hook==observer);Trace.hook=null;
        }
        System.out.println("attempts=2,visibility-generic-bridge=matched");
    }
}
