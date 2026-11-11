package fixture;
import java.lang.invoke.*;

@SuppressWarnings({"rawtypes","unchecked"})
public final class ConstructorOverloadArgumentHarness {
    static void check(boolean ok) {
        if(!ok)throw new AssertionError("owned constructor overload argument mismatch");
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        MethodHandle constructor=MethodHandles.lookup().findConstructor(Core.class,
            MethodType.methodType(void.class,Provider.class));
        for(int attempt=0;attempt<2;++attempt) {
            Observer o=new Observer(route,mode);
            Trace.hook=o;Trace.next=mode==3?null:mode==4?new Object():o.primary;
            Core result=null;Throwable caught=null;
            try {
                if(route==0)result=new Core(o.provider);
                else if(route==1)result=new Derived(o.provider);
                else result=(Core)constructor.invokeExact((Provider)o.provider);
                if(result.stored!=null) {
                    Value value=result.stored.value;
                    check(value==o.payload());
                }
            }catch(Throwable failure){caught=failure;}
            if(o.failure()!=null)check(caught==o.failure());
            else if(mode==1)check(caught!=null && caught.getClass()==NullPointerException.class);
            else if(mode==4 || mode==5)check(caught!=null && caught.getClass()==ClassCastException.class);
            else check(caught==null);
            check(o.events.equals(o.expectedEvents()));
            if(mode!=1 && mode!=4 && mode!=6 && mode!=7) {
                check(o.seen!=null);
                check(o.seen.value==(mode==8 || mode==9?null:o.id()));
                check(o.seen.stored==(mode>=8 && mode<=13?null:o.box()));
                check(o.seen.kind().equals(route==1?"derived":"base"));
            } else check(o.seen==null);
            if(caught==null || mode==5)check(result==o.seen);
        }
        System.out.println("attempts=2,constructor-overload-argument=matched");
    }
    public static final class Derived extends Core {
        Derived(Provider provider){super(provider);}
        @Override public String kind(){return "derived";}
    }
    public static final class Observer extends Trace {
        final int route,mode;
        final String initial,newId=new String("owned changed id");
        final Provider provider;
        final Box primary=new Box(),alternate=new Box();
        final Value original=new Value(new Object()),other=new Value(new Object()),changed=new Value(new Object());
        final Exception checked=new Exception("owned checked failure");
        final Error error=new LinkageError("owned error failure");
        final Object raw;
        Core seen;
        String events="";
        Observer(int route,int mode){
            this.route=route;this.mode=mode;
            initial=mode==2?null:new String("owned id");
            provider=mode==1?null:new Provider(initial);
            raw=mode==5?new String("owned raw element"):original;
            primary.value=raw;
            alternate.value=other;
        }
        String id(){return mode==19?newId:initial;}
        Box box(){return mode==3?null:mode==16?alternate:primary;}
        Object payload(){return mode==16?other:mode==18?changed:raw;}
        @Override public void observe(int phase,Object receiver,Object a,Object b){
            events+=phase+",";
            if(phase==0){
                check(receiver==null && a==null && b==null);
                if(mode==16)Trace.next=alternate;
            }else if(phase==5){
                check(receiver==provider && a==initial && b==null);
                if(mode==19)provider.id=newId;
            }else{
                Core current=(Core)receiver;
                if(seen==null)seen=current;
                check(current==seen && current.kind().equals(route==1?"derived":"base"));
                if(phase==1 || phase==3){
                    check(a==id() && b==null && current.stored==null);
                    check(current.value==(phase==1?null:id()));
                }else{
                    check((phase==2 || phase==4) && a==provider && b==box());
                    check(current.value==id() && current.stored==(phase==2?null:box()));
                    if(phase==2 && mode==17)Trace.next=alternate;
                    if(phase==4 && mode==18){Trace.next=alternate;primary.value=changed;}
                }
            }
            if(phase==0 && mode==6 || phase==1 && mode==8 || phase==3 && mode==10
                || phase==2 && mode==12 || phase==4 && mode==14)fail(checked);
            if(phase==5 && mode==7 || phase==1 && mode==9 || phase==3 && mode==11
                || phase==2 && mode==13 || phase==4 && mode==15)fail(error);
        }
        Throwable failure(){
            if(mode==6 || mode==8 || mode==10 || mode==12 || mode==14)return checked;
            if(mode==7 || mode==9 || mode==11 || mode==13 || mode==15)return error;
            return null;
        }
        String expectedEvents(){
            if(mode==4 || mode==6)return "0,";
            if(mode==1)return "0,";
            if(mode==7)return "0,5,";
            if(mode==8 || mode==9)return "0,5,1,";
            if(mode==10 || mode==11)return "0,5,1,3,";
            if(mode==12 || mode==13)return "0,5,1,3,2,";
            return "0,5,1,3,2,4,";
        }
        @SuppressWarnings("unchecked")
        static <T extends Throwable> void fail(Throwable failure)throws T{throw (T)failure;}
    }
}
