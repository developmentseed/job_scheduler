<?php
// $Id$

function job_scheduler_drush_help($section) {
  switch ($section) {
    case 'job_scheduler:job-scheduler-dispatch':
      return dt('Dispatch scheduled jobs, can be run instead of or in addition to cron.');
  }
}

/**
 * Implements hook_drush_command().
 */
function job_scheduler_drush_command() {
  $items = array();
  $items['job-scheduler-dispatch'] = array(
    'description' => 'Dispatch jobs, can be run instead of or in addition to cron.',
    'drupal dependencies' => array('job_scheduler'),
  );
  return $items;
}

/**
 * Dispatch jobs.
 */
function drush_job_scheduler_dispatch() {
  // Dispatch jobs.
  job_scheduler_cron();

  // Process queues used for scheduling.
  $info = job_scheduler_info();
  $scheduler_queues = array();
  foreach ($info as $schedule) {
    if ($schedule['queue name']) {
      $scheduler_queues[$schedule['queue name']] = $schedule['queue name'];
    }
  }

  $queues = module_invoke_all('cron_queue_info');
  drupal_alter('cron_queue_info', $queues);
  foreach ($queues as $queue_name => $info) {
    if (!in_array($queue_name, $scheduler_queues)) {
      continue;
    }
    $function = $info['worker callback'];
    $end = time() + (isset($info['time']) ? $info['time'] : 15);
    $queue = DrupalQueue::get($queue_name);
    while (time() < $end && ($item = $queue->claimItem())) {
      $function($item->data);
      $queue->deleteItem($item);
    }
  }
}
