import fixture.*;
import java.lang.invoke.*;

public final class BoundArrayConsumerHarness {
    static void check(boolean ok) {
        if (!ok) throw new AssertionError("owned bound array consumer mismatch");
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        MethodHandle call=MethodHandles.lookup().findStatic(Core.class,"call",
            MethodType.methodType(void.class,Consumer.class,Element[].class,int.class));
        for (int attempt=0;attempt<2;++attempt) {
            Observer observer=new Observer(mode);
            Trace.hook=observer; Trace.next=observer.offered;
            Throwable caught=null;
            try {
                if(route==0)Core.call(observer.receiver,observer.array,observer.flags);
                else if(route==1)Core.typed(observer.receiver,observer.array,observer.flags);
                else call.invokeExact((Consumer)observer.receiver,(Element[])observer.array,(int)observer.flags);
            }catch(Throwable failure){caught=failure;}
            if(observer.failure()!=null)check(caught==observer.failure());
            else if(mode==2 || mode==3)check(caught!=null && caught.getClass()==NullPointerException.class);
            else if(mode==4 || mode==5)check(caught!=null && caught.getClass()==ArrayIndexOutOfBoundsException.class);
            else if(mode==6)check(caught!=null && caught.getClass()==ClassCastException.class);
            else if(mode==8)check(caught!=null && caught.getClass()==ArrayStoreException.class);
            else check(caught==null);
            check(observer.events.equals(observer.expectedEvents()));
            if(observer.array!=null) {
                for(int i=0;i<observer.array.length;++i) {
                    Object wanted=observer.written() && i==observer.flags
                        ? observer.item() : observer.before[i];
                    check(observer.array[i]==wanted);
                }
            }
        }
        System.out.println("attempts=2,bound-array-consumer=matched");
    }
    public static final class Other implements Element {}
    public static final class SubConsumer extends Consumer {
        final Observer observer;
        SubConsumer(Observer observer){this.observer=observer;}
        @Override public <X extends Element> void accept(X[] values,int flags) {
            observer.events+="O,";
            super.accept(values,flags);
        }
    }
    public static final class Observer extends Trace {
        final int mode,flags;
        final Consumer receiver;
        final Element[] array;
        final Object[] before;
        final Object offered,changed=new Other();
        final Exception checked=new Exception("owned checked failure");
        final Error error=new LinkageError("owned error failure");
        String events="";
        Observer(int mode) {
            this.mode=mode; flags=mode==1?1:mode==5?-1:0;
            receiver=mode==2?null:mode==17?new SubConsumer(this):new Consumer();
            array=mode==3?null:mode==4?new Element[0]
                :mode==8?new Value[]{new Value(new Object()),new Value(new Object())}
                :new Element[]{mode==18?new Other():new Value(new Object()),new Value(new Object())};
            before=array==null?null:array.clone();
            offered=mode==6?new Object():mode==7?null:mode==8?new Other():new Value(new Object());
        }
        @Override public void observe(int phase,Object target,Object input,Object value,int count) {
            events+=phase+",";
            check(target==receiver && input==array && count==flags);
            if(phase==0 || phase==1) {
                check(value==null);
                if(phase==1 && mode==16)Trace.next=changed;
            }else {
                check((phase==2 || phase==3) && value==item());
                if(phase==2 && mode==15)Trace.next=changed;
            }
            if(phase==0 && mode==19 || phase==1 && mode==9
                || phase==2 && mode==11 || phase==3 && mode==13)fail(checked);
            if(phase==1 && mode==10 || phase==2 && mode==12 || phase==3 && mode==14)fail(error);
        }
        Object item(){return mode==16?changed:offered;}
        Throwable failure(){
            if(mode==9 || mode==11 || mode==13 || mode==19)return checked;
            if(mode==10 || mode==12 || mode==14)return error;
            return null;
        }
        boolean written(){
            return mode==0 || mode==1 || mode==7 || mode>=13 && mode<=18;
        }
        String expectedEvents(){
            String out="0,";
            if(mode==2 || mode==19)return out;
            if(mode==17)out+="O,";
            out+="1,";
            if(mode==6 || mode==9 || mode==10)return out;
            out+="2,";
            if(mode==3 || mode==4 || mode==5 || mode==8 || mode==11 || mode==12)return out;
            return out+"3,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure)throws T{throw (T)failure;}
    }
}
