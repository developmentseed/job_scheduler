<?php
// $Id: JobScheduler.inc,v 1.11 2010/09/28 22:11:44 alexb Exp $

/**
 * @file
 * JobScheduler class.
 */

/**
 * Use to make Job Scheduler exceptions identifiable by type.
 */
class JobSchedulerException extends Exception {}

/**
 * Manage scheduled jobs.
 */
class JobScheduler {
  /**
   * The name of this scheduler.
   */
  protected $name;

  /**
   * Produces a single instance of JobScheduler for a schedule name.
   */
  public static function get($name) {
    static $schedulers;
    // Instantiante a new scheduler for $name if we haven't done so yet.
    if (!isset($schedulers[$name])) {
      $class = variable_get('job_scheduler_class_'. $name, 'JobScheduler');
      $schedulers[$name] = new $class($name);
    }
    return $schedulers[$name];
  }

  /**
   * Creates a JobScheduler object.
   */
  protected function __construct($name) {
    $this->name = $name;
  }

  /**
   * Returns scheduler info.
   *
   * @see hook_cron_job_scheduler_info().
   *
   * @throws JobSchedulerException.
   */
  public function info() {
    if ($info = job_scheduler_info($this->name)) {
      return $info;
    }
    throw new JobSchedulerException('Could not find Job Scheduler cron information for '. check_plain($this->name));
  }

  /**
   * Add a job to the schedule, replace any existing job.
   *
   * A job is uniquely identified by $job = array(type, id).
   *
   * @param $job
   *   An array that must contain the following keys:
   *   'type'     - A string identifier of the type of job.
   *   'id'       - A numeric identifier of the job.
   *   'period'   - The time when the task should be executed.
   *   'periodic' - True if the task should be repeated periodically.
   *
   * @code
   * function worker_callback($job) {
   *   // Work off job.
   *   // Set next time to be called. If this portion of the code is not
   *   // reached for some reason, the scheduler will keep periodically invoking
   *   // the callback() with the period value initially specified.
   *   $scheduler->set($job);
   * }
   * @endcode
   */
  public function set($job) {
    $job['name'] = $this->name;
    $job['last'] = REQUEST_TIME;
    if (!empty($job['crontab'])) {
      $crontab = new JobSchedulerCronTab($job['crontab']);
      $job['next'] = $crontab->next_time(REQUEST_TIME);      
    }
    else {
      $job['next'] = REQUEST_TIME + $job['period'];
    }
    $job['scheduled'] = 0;
    $this->remove($job);
    drupal_write_record('job_schedule', $job);
  }

  /**
   * Reserve a job.
   */
  protected function reserve($job) {
    $job['name'] = $this->name;
    $job['scheduled'] =
    $job['last'] = REQUEST_TIME;
    $job['next'] = $job['period'] + REQUEST_TIME;
    drupal_write_record('job_schedule', $job, array('name', 'type', 'id'));
  }

  /**
   * Remove a job from the schedule, replace any existing job.
   *
   * A job is uniquely identified by $job = array(type, id).
   */
  public function remove($job) {
    db_delete('job_schedule')
      ->condition('name', $this->name)
      ->condition('type', $job['type'])
      ->condition('id', isset($job['id']) ? $job['id'] : 0)
      ->execute();
  }

  /**
   * Remove all jobs for a given type.
   */
  public function removeAll($type) {
    db_delete('job_schedule')
      ->condition('name', $this->name)
      ->condition('type', $type)
      ->execute();
  }

  /**
   * Dispatches a job.
   *
   * Executes a worker callback or if schedule declares a queue name, queues a
   * job for execution.
   *
   * @param $job
   *   A $job array as passed into set() or read from job_schedule table.
   *
   * @throws Exception
   *   Exceptions thrown by code called by this method are passed on.
   */
  public function dispatch($job) {
    $info = $this->info();
    if (!$job['periodic']) {
      $this->remove($job);
    }
    if ($info['queue name']) {
      if (DrupalQueue::get($info['queue name'])->createItem($job)) {
        $this->reserve($job);
      }
    }
    else {
      $this->execute($job, $info);
    }
  }

  /**
   * Executes a job that is no
   * 
   * @param $job
   *   A $job array as passed into set() or read from job_schedule table.
   */
  public function execute($job) {
    if (function_exists($info['worker callback'])) {
      // If the job is periodic, re-schedule it before calling the worker, just in case
      if ($job['periodic']) {
        $this->reschedule($job);
      }
      $info['worker callback']($job); 
    }
    else {
      // @todo If worker doesn't exist anymore we should do something about it, remove and throw exception?
      $this->remove($job);
      throw new JobSchedulerException('Could not find worker callback function: '. $info['worker callback']);
    }    
  }
  
  /**
   * Re-schedule a job
   */
  public function reschedule($job) {
    $info = $this->info();
    $job['last'] = REQUEST_TIME;
    if (isset($info['jobs'][$job['id']]['crontab'])) {
      $crontab = new JobSchedulerCronTab($info['jobs'][$job['id']]['crontab']);
      $job['next'] = $crontab->next_time(REQUEST_TIME);
    }
    else {
      $job['next'] = REQUEST_TIME + $job['period'];
    }
    drupal_write_record('job_schedule', $job, array('name', 'type', 'id'));
  }
  
  /**
   * Check whether a job exists in the queue
   */
  public function check($info) {
    return db_select('job_schedule')
      ->condition('name', $this->name)
      ->condition('type', $job['type'])
      ->condition('id', isset($job['id']) ? $job['id'] : 0)
      ->execute()
      ->fetchObject();    
  }
}

/**
 * Jose's cron tab parser = Better try only simple crontab strings.
 * 
 * Usage:
 *   // Run 23 minutes after midn, 2am, 4am ..., everyday
 *   $crontab = new JobSchedulerCronTab('23 0-23/2 * * *');
 *   // When this needs to run next, from current time?
 *   $next_time = $crontab->next_time(time());
 * 
 * I hate Sundays.
 */
class JobSchedulerCronTab {
  // Original crontab elements
  public $crontab;
  // Parsed numeric values indexed by type
  public $cron;

  /**
   * Constructor
   * 
   * @param $crontab string
   *   Crontab text line
   */
  public function __construct($crontab) {
    $this->crontab = $crontab;
    $this->cron = $this->parse($crontab);
  }

  /**
   * Parse full crontab string into an array of type => values
   * 
   * Note this one is static and can be used to validate values
   */
  public static function parse($crontab) {
    // Crontab elements, names match PHP date indexes (getdate)
    $keys = array('minutes', 'hours', 'mday', 'mon', 'wday');
    // Replace multiple spaces by single space
    $crontab = preg_replace('/(\s+)/', ' ', $crontab);
    // Expand into elements and parse all
    $values = explode(' ', trim($crontab));
    $elements = array();
    while (($type = array_shift($keys)) && ($data = array_shift($values))) {
      $elements[$type] = self::parse_element($type, $data, TRUE);
    }
    // Return only if we have the right number of elements
    // Dangerous means works running every second or things like that.
    if (count(array_filter($elements)) == 5) {
      return $elements;
    }
    else {
      return NULL;
    }
  }
  
  /**
   * Find the next occurrence as unix timestamp
   * 
   * @param $start_time timestamp
   *   Starting time
   */
  public function next_time($start_time = NULL) {
    $start_time = isset($start_time) ? $start_time : time();
    $start_date = getdate($start_time); // Get minutes, hours, mday, wday, mon, year
    if ($date = next_date($start_date)) {
      return mktime($date['hours'], $date['minutes'], 0, $date['month'], $date['day'], $date['year']);
    }
    else {
      return 0;
    }
  }

  /**
   * Find the next occurrence as a date array
   * 
   * @see 
   * @param $date
   *   Date array as returned by
   */
  public function next_date($date) {
    $date['seconds'] = 0;
    // It is possible that the current date doesn't match 
    if ($this->check_day($date) && ($nextdate = $this->next_hour($date))) {
      return $nextdate;
    }
    elseif ($nextdate = $this->next_day($date)) {
      return $nextdate;
    }
    else {
      return FALSE;
    }
  }

  /**
   * Check whether date's day is a valid one
   */
  protected function check_day($date) {
    foreach (array('wday', 'mday', 'month') as $key) {
      if (!in_array($date[$key], $this->cron[$key])) {
        return FALSE;
      }
    }
    return TRUE;
  }

  /**
   * Find the next day from date that matches with cron parameters
   */
  protected function next_day($date) {
    $i = 0; // Safety check, we love infinite loops...
    while ($i++ < 367) {
      // This should fix values out of range, like month > 12, day > 31....
      // So we can trust we get the next valid day, can't we?
      $time = mktime(0, 0, 0, $date['month'], $date['day'] + 1, $date['year']);
      $date = getdate($time);
      if (check_day($date)) {
        $date['hours'] = reset($this->cron['hours']);
        $date['minutes'] = reset($this->cron['minutes']);
        return $date;
      }
    }
  }
  /**
   * Find the next available hour within the same day
   */
  protected function next_hour($date) {
    $cron = $this->cron;
    while ($hour = array_shift($crom['hours'])) {
      if ($date['hour'] <= $hour) {
        while ($minute = array_shift($cron['minutes'])) {
          if ($date['minutes'] < $minute) {
            $date['hour'] = $hour;
            $date['minutes'] = $minute;
            return $date;
          }
        }
      }
    }
    return FALSE;
  }
  
  /**
   * Parse each text element. Recursive up to some point...
   */
  protected static function parse_element($type, $string, $translate = FALSE) {
    $string = trim($string);
    if ($translate) {
      $string = self::translate_names($type, $string);
    }
    if ($string === '*') {
      // This means all possible values, return right away, no need to double check
      return self::possible_values($type);
    }
    elseif (strpos($string, '/')) {
      // Multiple. Example */2, for weekday will expand into 2, 4, 6 
      list($values, $multiple) = explode('/', $string);
      $values = self::parse_element($type, $values);
      foreach ($values as $value) {
        if (!($value % $multiple)) {
          $range[] = $value;
        }
      }
    }
    elseif (strpos($string, ',')) {
      // Now process list parts, expand into items, process each and merge back
      $list = explode(',', $string);
      $range = array();
      foreach ($list as $item) {
        if ($values = self::parse_element($type, $item)) {
          $range = array_merge($range, $values);
        }
      }
    }
    elseif (strpos($string, '-')) {
      // This defines a range. Example 1-3, will expand into 1,2,3
      list($start, $end) = explode('-', $string);
      // Double check the range is within possible values
      $range = range($start, $end);
    }
    elseif (is_numeric($string)) {
      // This looks like a single number, double check it's int
      $range = array((int)$string);
    }

    // Return unique sorted values and double check they're within possible values
    if (!empty($range)) {
      $range = array_intersect(array_unique($range), self::possible_values($type));
      sort($range);
      // Sunday validation. We need cron values to match PHP values, thus week day 7 is not allowed, must be 0 
      if ($type == 'wday' && in_array(7, $range)) {
        array_pop($range);
        array_unshift($range, 0);
      } 
      return $range;
    }
    else {
      // No match found for this one, will produce an error with validation
      return array();
    }
  }

  /**
   * Get values for each type
   */
  public static function possible_values($type) {
    switch ($type) {
      case 'minutes':
        return range(0, 59);
      case 'hours':
        return range(0, 23);
      case 'mday':
        return range(1, 31);
      case 'mon':
        return range(1, 12);
      case 'wday':
        // These are PHP values, not *nix ones
        return range(0, 6);
        
    }
  }

  /**
   * Replace element names by values
   */
  public static function translate_names($type, $string) {
    switch ($type) {
      case 'wday':
        $replace = array_merge(
          // Tricky, tricky, we need sunday to be zero at the beginning of a range, but 7 at the end
          array('-sunday' => '-7', '-sun' => '-7', 'sunday-' => '0-', 'sun-' => '0-'),
          array_flip(array('sunday', 'monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday')),
          array_flip(array('sun', 'mon', 'tue', 'wed', 'thu', 'fri', 'sat'))
        );
        break;
      case 'mon':
        $replace = array_merge(
          array_flip(array('nomonth1', 'january', 'february', 'march', 'april', 'may', 'june', 'july', 'august', 'september', 'october', 'november', 'december')),
          array_flip(array('nomonth2', 'jan', 'feb', 'mar', 'apr', 'may', 'jun', 'jul', 'aug', 'sep', 'oct', 'nov', 'dec')),
          array('sept' => 9)
        );
        break;
    }
    if (empty($replace)) {
      return $string;
    }
    else {
      return strtr($string, $replace);
    }
  }
}