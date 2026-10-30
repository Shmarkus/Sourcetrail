package example;

public class Caller implements Greeter {
    @Override
    public String greeting(Person person) {
        return person.getName();
    }

    public String greeting(Person person, String salutation) {
        return salutation + person.getName().length();
    }
}
