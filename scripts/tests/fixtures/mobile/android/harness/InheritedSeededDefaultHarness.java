import fixture.*;
import kotlin.jvm.functions.Function2;
import java.lang.reflect.*;
public final class InheritedSeededDefaultHarness {
    static int mode,direction;
    static Object receiver,seed,operation,result;
    static Throwable failure;
    static void require(boolean ok){if(!ok)throw new AssertionError("owned inherited seeded default");}
    static final class Operand {
        public String toString(){throw new AssertionError("owned seed effect");}
        public int hashCode(){throw new AssertionError("owned seed effect");}
    }
    static final class Probe implements Function2<Object,Object,Object> {
        public Object invoke(Object a,Object b){return observe(a,b);}
        public String toString(){throw new AssertionError("owned operation effect");}
        public int hashCode(){throw new AssertionError("owned operation effect");}
    }
    public static Object observe(Object a,Object b){
        require(a==(direction==0?seed:receiver) && b==(direction==0?receiver:seed));
        require(Trace.receiver==receiver && Trace.seed==seed && Trace.operation==operation && Trace.events==12 && Trace.enters==1 && Trace.callbacks==0 && Trace.virtuals==0);
        Trace.callbacks++;Trace.events=123;
        if(mode==8)throw (RuntimeException)failure;
        if(mode==9)throw (Error)failure;
        return result;
    }
    static void formal(TypeVariable<?> variable,GenericDeclaration declaration){require(variable.getGenericDeclaration().equals(declaration) && variable.getBounds().length==1 && variable.getBounds()[0]==Object.class);}
    static void callback(Type type,TypeVariable<?> r){
        ParameterizedType p=(ParameterizedType)type;require(p.getRawType()==Function2.class);Type[] args=p.getActualTypeArguments();require(args.length==3);
        for(int i=0;i<3;i++){
            WildcardType w=(WildcardType)args[i];Type expected=i==2?r:((direction==0?i==0:i==1)?r:Parent.class);
            if(i==2)require(w.getLowerBounds().length==0 && w.getUpperBounds().length==1 && w.getUpperBounds()[0].equals(expected));
            else require(w.getLowerBounds().length==1 && w.getLowerBounds()[0].equals(expected) && w.getUpperBounds()[0]==Object.class);
        }
    }
    static void metadata(String accessor,String forward,boolean projected)throws Exception{
        TypeVariable<?>[] owner=Child.class.getTypeParameters();require(owner.length==1);formal(owner[0],Child.class);
        Method raw=Child.class.getDeclaredMethod(accessor,Child.class,Object.class,Function2.class);require(raw.getReturnType()==Object.class && raw.getTypeParameters().length==0 && raw.isSynthetic()==!projected);
        require(raw.getGenericParameterTypes()[0]==Child.class && raw.getGenericParameterTypes()[1]==Object.class && raw.getGenericParameterTypes()[2]==Function2.class);
        String name=direction==0?"foldIn":"foldOut";
        Method parent=Parent.class.getDeclaredMethod(name,Object.class,Function2.class);require(parent.isDefault() && parent.getTypeParameters().length==1);TypeVariable<?> r=parent.getTypeParameters()[0];formal(r,parent);
        require(parent.getGenericParameterTypes()[0].equals(r) && parent.getGenericReturnType().equals(r));callback(parent.getGenericParameterTypes()[1],r);
        Method entry=Class.forName("fixture.Child$DefaultImpls").getDeclaredMethod(forward,Child.class,Object.class,Function2.class);TypeVariable<?>[] vars=entry.getTypeParameters();require(vars.length==2);formal(vars[0],entry);formal(vars[1],entry);
        ParameterizedType self=(ParameterizedType)entry.getGenericParameterTypes()[0];require(self.getRawType()==Child.class && self.getActualTypeArguments()[0].equals(vars[0]));
        require(entry.getGenericParameterTypes()[1].equals(vars[1]) && entry.getGenericReturnType().equals(vars[1]));callback(entry.getGenericParameterTypes()[2],vars[1]);
    }
    public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),alias=Integer.parseInt(args[2]);direction=Integer.parseInt(args[1]);mode=Integer.parseInt(args[3]);metadata(args[4],args[5],Boolean.parseBoolean(args[6]));
        for(int attempt=0;attempt<2;attempt++){
            Trace.enters=Trace.callbacks=Trace.virtuals=Trace.events=Trace.fail=0;Trace.receiver=Trace.seed=Trace.operation=null;
            failure=mode==9?new AssertionError("owned exact error"):new IllegalArgumentException("owned exact failure");Trace.failure=failure instanceof RuntimeException?(RuntimeException)failure:null;
            receiver=alias==0?new InheritedSeededDefaultCaller.Receiver():new InheritedSeededDefaultCaller.Derived();operation=alias==0?new Probe():receiver;seed=new Operand();
            if(mode==1)seed=null;if(mode==2)seed=receiver;if(mode==3)seed=operation;if(mode==14)seed="owned seed";
            result=mode==4?null:mode==5?receiver:mode==6?operation:mode==7?new Operand():mode==14?Integer.valueOf(173):seed;
            if(mode==10)operation=null;if(mode==11)receiver=null;Trace.fail=mode==12?1:mode==13?2:0;
            Object value=null;Throwable seen=null;try{value=InheritedSeededDefaultCaller.call(route,direction,(Child)receiver,seed,(Function2)operation,args[4],args[5]);}catch(Throwable error){seen=error;}
            if(mode<=7 || mode==14)require(seen==null && value==result);
            else if(mode==10 || mode==11)require(seen!=null && seen.getClass()==NullPointerException.class);
            else require(seen==failure);
            int enters=mode==11?0:1,calls=mode<=9 || mode==14?1:0,events=mode==11?0:mode==12?1:(calls==1?123:12);
            require(Trace.enters==enters && Trace.callbacks==calls && Trace.events==events && Trace.virtuals==0 && Trace.receiver==receiver);
            require(Trace.seed==(mode==11 || mode==12?null:seed) && Trace.operation==(mode==11 || mode==12?null:operation));
        }
        System.out.println("attempts=2,inherited-seeded-default=matched");
    }
}
